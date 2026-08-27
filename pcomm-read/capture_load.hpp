#pragma once
// Reading a capture directory into DuckDB — the step that turns `.pcs` streams
// into queryable tables.
//
// WHY IT IS A HEADER. It was inside pcomm-read's `export_parquet`, which meant
// the only way to look at a capture was to write Parquet to disk first and
// point a second tool at the output. That is a fine pipeline for a fixture and
// a poor one for "show me this run": `pcomm-ui <capture>` should just work, and
// it cannot without this loop. Same reasoning that moved `duck_mount.hpp` and
// `link_sql.hpp` out — two copies of a policy is one copy and one bug.
//
// Note the asymmetry with duck_mount.hpp, which is deliberate and is the reason
// they are separate files. This one reads STREAMS and creates a table only for
// what the capture actually contains: a missing stream produces no table,
// because "absent" and "empty" say different things about a run (under IBGDA
// the ib_* streams are legitimately absent). duck_mount.hpp reads PARQUET and
// does the opposite — it manufactures empty tables for absent streams, so that
// panels do not each have to branch on which provider ran. Loading is where the
// distinction still exists; mounting is where we have decided to erase it.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "pcomm/rec/intern_reader.hpp"
#include "pcomm/rec/stream_reader.hpp"

// At FILE SCOPE, before any namespace is opened. duck_mount.hpp reaches
// raw_columns.hpp, which opens `namespace pcomm::raw`; including it inside
// `namespace pcomm::capture` would declare a nested pcomm::capture::pcomm that
// shadows the real one. That has already happened once in this codebase (see
// pcomm-read/main.cpp's include block) — it is a confusing failure and it is
// only avoidable by convention, so the convention is stated at both sites.
#if PCOMM_HAVE_DUCKDB
#include "duck_mount.hpp"
#endif

namespace pcomm::capture {

namespace fs = std::filesystem;

// Apply `f.operator()<T>()` once per type in a tuple. The record tables are a
// type list, and every exporter is a fold over it — which is what keeps "add a
// record type" from meaning "edit six switch statements".
template<typename Tuple, typename F, std::size_t... Is>
void for_each_type_impl(F&& f, std::index_sequence<Is...>) {
    (f.template operator()<std::tuple_element_t<Is, Tuple>>(), ...);
}
template<typename Tuple, typename F> void for_each_type(F&& f) {
    for_each_type_impl<Tuple>(std::forward<F>(f),
                              std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

// One rank's capture: streams + intern dictionaries.
struct RankDir {
    int                   rank{0};
    fs::path              dir;
    rec::InternDictionary intern;  // pcomm/rec/intern_reader.hpp

    const char* str(core::Handle h) const { return intern.str(h); }
    const char* str(rec::InternDomain d, core::Handle h) const { return intern.str(d, h); }
};

// A rank's stream file for record type R.
template<core::Record R> fs::path stream_path(const RankDir& r) {
    return rec::stream_path<R>(r.dir);
}

// Find every rank-* directory under `root`. A single run lays them out flat
// (root/rank-*); a replayed run nests one level per pass (root/<pass>/rank-*),
// e.g. root/ib/rank-0 and root/gpumem/rank-0 for the same logical rank 0. We
// recurse and accept duplicate rank numbers across passes — each RankDir loads
// its own intern table, so pass-local handles never collide, and because each
// provider emits a disjoint record-type range the passes merge by rank number.
inline void scan_ranks_into(const fs::path& root, std::vector<RankDir>& ranks) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory())
            continue;
        auto name = entry.path().filename().string();
        if (name.rfind("rank-", 0) != 0) {
            scan_ranks_into(entry.path(), ranks);  // descend into a pass subdir
            continue;
        }
        RankDir r;
        r.rank = std::atoi(name.c_str() + 5);
        r.dir = entry.path();
        r.intern = rec::InternDictionary::load(r.dir);
        ranks.push_back(std::move(r));
    }
}

inline std::vector<RankDir> scan_ranks(const fs::path& root) {
    std::vector<RankDir> ranks;
    scan_ranks_into(root, ranks);
    return ranks;
}

// True when `root` looks like a capture rather than an export directory — used
// to decide how to interpret a path the user gave us. Cheap on purpose: it is a
// dispatch hint, and the load below reports properly if it is wrong.
inline bool looks_like_capture(const fs::path& root) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory())
            continue;
        if (e.path().filename().string().rfind("rank-", 0) == 0)
            return true;
        for (const auto& n : fs::directory_iterator(e.path(), ec))
            if (n.is_directory() && n.path().filename().string().rfind("rank-", 0) == 0)
                return true;
    }
    return false;
}

#if PCOMM_HAVE_DUCKDB

struct LoadStats {
    std::size_t tables{0};
    std::size_t rows{0};
    std::size_t failed{0};
};

// Create and populate one DuckDB table per record stream present in `ranks`.
//
// Values go to the appender as themselves — the column types come from the
// record declarations via the generated raw_columns.hpp, so nothing is
// stringified on the way in and nothing has to be re-inferred on the way out.
template<typename Records = raw::RawRecords>
LoadStats load_raw_tables(duckdb_connection con, const std::vector<RankDir>& ranks) {
    LoadStats st;
    for_each_type<Records>([&]<typename R>() {
        using Raw = raw::RawTable<R>;

        // Existence first, so an absent stream produces no table at all rather
        // than an empty one. Only existence, though: loading every rank up
        // front to count rows held the whole capture in memory at once (600 MB
        // peak on a 167 MB capture) for no gain, since each rank is appended
        // and dropped below.
        const bool any = std::any_of(ranks.begin(), ranks.end(), [](const RankDir& r) {
            return fs::exists(stream_path<R>(r));
        });
        if (!any)
            return;

        if (!duck::exec(con, Raw::ddl, Raw::table)) {
            st.failed++;
            return;
        }

        duckdb_appender ap{};
        if (duckdb_appender_create(con, nullptr, Raw::table, &ap) != DuckDBSuccess) {
            std::fprintf(stderr, "pcomm: cannot append to %s\n", Raw::table);
            st.failed++;
            return;
        }
        // One rank at a time: the vector is freed before the next is read, so
        // peak memory is the largest single stream rather than the capture.
        std::size_t rows = 0;
        for (const auto& r : ranks) {
            const auto path = stream_path<R>(r);
            if (!fs::exists(path))
                continue;
            const auto recs = rec::load_stream<R>(path);
            rows += recs.size();
            for (const auto& s : recs) {
                Raw::append(ap, r.rank, s);
                duckdb_appender_end_row(ap);
            }
        }
        // Flush before destroy so a write error is reported rather than
        // swallowed by the destructor path.
        const bool flushed = duckdb_appender_flush(ap) == DuckDBSuccess;
        if (!flushed)
            std::fprintf(stderr, "pcomm: %s: appender flush failed: %s\n", Raw::table,
                         duckdb_appender_error(ap));
        duckdb_appender_destroy(&ap);
        if (!flushed) {
            st.failed++;
            return;
        }
        st.tables++;
        st.rows += rows;
    });
    return st;
}

// Load every rank's intern dictionary into one `intern_text` table.
//
// The dictionaries are already loaded — `scan_ranks_into` reads one per RankDir
// — so this only reshapes them. Returns the row count.
//
// COLLISIONS ARE NOT RESOLVED HERE, and that is deliberate. A REPLAYED run
// nests one rank-* directory per pass (root/ib/rank-0, root/gpumem/rank-0) and
// both are RankDir rank 0 with independent dictionaries, so the same
// (rank, domain, handle) can carry two different strings in one capture. The
// loader's job is to report what is on disk; deciding what an ambiguous handle
// means is a semantic question, and link_sql.hpp's `_intern_name` answers it
// the way this codebase answers every other one — by refusing to resolve and
// counting the refusal, rather than by picking whichever sorted first.
inline std::size_t load_intern_text(duckdb_connection con, const std::vector<RankDir>& ranks) {
    if (!duck::exec(con, duck::kInternTextDdl, duck::kInternTextTable))
        return 0;
    duckdb_appender ap{};
    if (duckdb_appender_create(con, nullptr, duck::kInternTextTable, &ap) != DuckDBSuccess) {
        std::fprintf(stderr, "pcomm: cannot append to %s\n", duck::kInternTextTable);
        return 0;
    }
    std::size_t rows = 0;
    for (const auto& r : ranks) {
        r.intern.for_each_string([&](rec::InternDomain d, core::Handle h, const std::string& text) {
            duckdb_append_int32(ap, r.rank);
            duckdb_append_uint8(ap, static_cast<std::uint8_t>(d));
            duckdb_append_uint32(ap, h);
            duckdb_append_varchar(ap, text.c_str());
            duckdb_appender_end_row(ap);
            rows++;
        });
    }
    const bool flushed = duckdb_appender_flush(ap) == DuckDBSuccess;
    if (!flushed)
        std::fprintf(stderr, "pcomm: %s: appender flush failed: %s\n", duck::kInternTextTable,
                     duckdb_appender_error(ap));
    duckdb_appender_destroy(&ap);
    return flushed ? rows : 0;
}

// Create an EMPTY table for every raw stream the capture did not contain.
//
// This is the point where the absent/empty distinction is deliberately
// discarded, and it belongs to the consumer rather than the loader. A reader
// exporting Parquet wants absent streams to stay absent — that is information
// about the run. A UI does not: every panel naming a stream some provider
// happens not to emit would otherwise fail outright, and each would have to
// branch on which provider ran. Same policy duck_mount.hpp applies to a
// Parquet export, applied to a freshly loaded capture.
inline void fill_missing_raw_tables(duckdb_connection con) {
    for_each_type<raw::RawRecords>([&]<typename R>() {
        using Raw = raw::RawTable<R>;
        duckdb_result probe{};
        const bool    present =
            duckdb_query(con, ("SELECT 1 FROM \"" + std::string(Raw::table) + "\" LIMIT 0").c_str(),
                         &probe) == DuckDBSuccess;
        duckdb_destroy_result(&probe);
        if (!present)
            duck::exec(con, Raw::ddl, Raw::table);
    });
}

#endif  // PCOMM_HAVE_DUCKDB

}  // namespace pcomm::capture
