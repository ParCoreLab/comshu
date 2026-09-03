// Mounting an export into DuckDB — shared by `pcomm-read --link` and `pcomm-ui`.
//
// Shared rather than copied because the interesting part is a *policy*, and two
// copies of a policy is one copy and one bug: a missing stream must become an
// EMPTY table with the right columns, not a missing one.
//
// Why that policy exists. A capture legitimately lacks whole families of
// streams — a gpumem-only run has no `ib_modify_qp`, and under IBGDA the entire
// `ib_*` family is absent while `mlx5_*` carries everything. Queries above this
// layer union both transports, so they name tables that no single-provider
// capture has. Without the empty stand-in, every panel and every derived query
// would have to branch on which provider produced the capture, which is the
// coupling the whole derived layer exists to remove.
//
// The distinction between "absent" and "empty" is preserved where it matters:
// the exporter still writes no file for a stream that does not exist. It is
// erased only here, at the point of query.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

extern "C" {
#include "duckdb.h"
}
#include "raw_columns.hpp"

namespace pcomm::duck {

namespace fs = std::filesystem;

/** One SQL statement, with the error text surfaced rather than dropped. */
inline bool exec(duckdb_connection con, const std::string& sql, const char* what) {
    duckdb_result res;
    const bool    ok = duckdb_query(con, sql.c_str(), &res) == DuckDBSuccess;
    if (!ok) {
        const char* err = duckdb_result_error(&res);
        std::fprintf(stderr, "pcomm: %s failed: %s\n", what, err ? err : "(no message)");
    }
    duckdb_destroy_result(&res);
    return ok;
}

namespace detail {
template<typename Tuple, typename F, std::size_t... Is>
void for_each_type_impl(F&& f, std::index_sequence<Is...>) {
    (f.template operator()<std::tuple_element_t<Is, Tuple>>(), ...);
}
}  // namespace detail

template<typename Tuple, typename F> void for_each_type(F&& f) {
    detail::for_each_type_impl<Tuple>(std::forward<F>(f),
                                      std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

// The intern dictionary, AS A TABLE.
//
// NOT `intern_string`, which is already taken — that is the raw stream of
// `InternString` INDEX records (offset, length, handle, domain), and it is
// useless on its own because the bytes it points into live in a separate blob.
// This is the resolved form: the text itself. The two are one letter apart in
// the same database on purpose, so `intern_text` is the one with the answer.
//
// Not a `PCOMM_RECORD` and so not in `raw::RawRecords`: the on-disk form is an
// index stream plus a byte blob, and neither is fixed-stride. It is mounted
// beside the raw tables anyway because the derived layer's job includes
// decoding — `export_parquet`'s header says resolved intern handles "belong to
// the derived-table layer, and SQL is a better place for them than a C++
// printer" — and that layer had no dictionary to decode WITH.
//
// `rank` is part of the key and must stay in every join. Handles restart at 1
// in every domain of every rank, so handle 3 on rank 0 and handle 3 on rank 1
// are different strings; this is the same "a wire id is only unique with its
// scope" rule link_sql.hpp opens with, arriving on a different id.
constexpr const char* kInternTextTable = "intern_text";
constexpr const char* kInternTextDdl =
    "CREATE OR REPLACE TABLE \"intern_text\" ("
    "rank INTEGER, domain UTINYINT, handle UINTEGER, text VARCHAR)";

// What the READER found on disk, one row per rank. The companion to
// `capture_status`, which is what the WRITER attested to, and the two are
// separate tables because they are separate witnesses: the writer knows what it
// failed to write, the reader knows what is no longer there. A capture damaged
// after the run — an interrupted copy off a cluster, a truncated archive — is
// invisible to the first and obvious to the second.
//
// Not a PCOMM_RECORD for the same reason `intern_text` is not: it describes the
// capture rather than being part of it, and it does not exist until something
// reads the files.
//
// `streams_found` counts *.pcs in the rank directory, NOT streams successfully
// loaded, and not only streams of a type this build knows. Counting files is
// what makes it comparable with capture_status.streams_open, which is also a
// count of files the writer framed.
constexpr const char* kCaptureIntegrityTable = "capture_integrity";
constexpr const char* kCaptureIntegrityDdl =
    "CREATE OR REPLACE TABLE \"capture_integrity\" ("
    "rank INTEGER, streams_found INTEGER, streams_unreadable INTEGER, "
    "streams_truncated INTEGER, streams_foreign_schema INTEGER)";

/**
 * Mount reader-side integrity from an export, empty when it is absent.
 */
inline bool mount_capture_integrity(duckdb_connection con, const fs::path& dir) {
    const auto parquet = dir / (std::string(kCaptureIntegrityTable) + ".parquet");
    const auto csv = dir / (std::string(kCaptureIntegrityTable) + ".csv");
    if (fs::exists(parquet))
        return exec(con,
                    "CREATE OR REPLACE VIEW \"" + std::string(kCaptureIntegrityTable) +
                        "\" AS SELECT * FROM read_parquet('" + parquet.string() + "')",
                    kCaptureIntegrityTable);
    if (fs::exists(csv))
        return exec(con, kCaptureIntegrityDdl, kCaptureIntegrityTable) &&
               exec(con,
                    "COPY \"" + std::string(kCaptureIntegrityTable) + "\" FROM '" + csv.string() +
                        "' (FORMAT CSV, HEADER)",
                    kCaptureIntegrityTable);
    return exec(con, kCaptureIntegrityDdl, kCaptureIntegrityTable);
}

/**
 * Mount the intern dictionary from an export, empty when it is absent.
 *
 * Same absent-becomes-empty policy as the raw tables, and for the same reason:
 * a derived query that LEFT JOINs a name must run against a capture exported by
 * an older build, resolving nothing rather than failing to parse.
 */
inline bool mount_intern_text(duckdb_connection con, const fs::path& dir) {
    const auto parquet = dir / (std::string(kInternTextTable) + ".parquet");
    const auto csv = dir / (std::string(kInternTextTable) + ".csv");
    if (fs::exists(parquet))
        return exec(con,
                    "CREATE OR REPLACE VIEW \"" + std::string(kInternTextTable) +
                        "\" AS SELECT * FROM read_parquet('" + parquet.string() + "')",
                    kInternTextTable);
    if (fs::exists(csv))
        return exec(con, kInternTextDdl, kInternTextTable) &&
               exec(con,
                    "COPY \"" + std::string(kInternTextTable) + "\" FROM '" + csv.string() +
                        "' (FORMAT CSV, HEADER)",
                    kInternTextTable);
    return exec(con, kInternTextDdl, kInternTextTable);
}

/**
 * Make every raw table reachable by name. Returns how many were actually
 * present; the rest are empty tables with the correct columns.
 *
 * Parquet is preferred and CSV accepted, so a `--csv` export is queryable too.
 * CSV is loaded through the generated DDL and COPY FROM rather than read_csv's
 * auto-detect: the types are declared by the record struct, and inferring them
 * from text is what once made `mem_object.base` a BIGINT on one capture and a
 * VARCHAR on another.
 */
inline std::size_t mount_raw_tables(duckdb_connection con, const fs::path& dir) {
    std::size_t present = 0;
    for_each_type<raw::RawRecords>([&]<typename R>() {
        using Raw = raw::RawTable<R>;
        const auto name = std::string(Raw::table);
        const auto parquet = dir / (name + ".parquet");
        const auto csv = dir / (name + ".csv");
        if (fs::exists(parquet)) {
            exec(con,
                 "CREATE OR REPLACE VIEW \"" + name + "\" AS SELECT * FROM read_parquet('" +
                     parquet.string() + "')",
                 Raw::table);
            present++;
        } else if (fs::exists(csv)) {
            if (exec(con, Raw::ddl, Raw::table) &&
                exec(con, "COPY \"" + name + "\" FROM '" + csv.string() + "' (FORMAT CSV, HEADER)",
                     Raw::table))
                present++;
        } else {
            exec(con, Raw::ddl, Raw::table);
        }
    });
    // Mounted HERE rather than left to each caller: `pcomm link`, `pcomm sql`
    // and `pcomm ui` all reach the derived layer through this one function, and
    // a name resolution that worked in one of the three would be worse than one
    // that worked in none. Not counted in `present` — it is not a raw stream,
    // and the number is printed as "raw tables".
    mount_intern_text(con, dir);
    // Same reasoning, and it matters more here: link_health's first two rows are
    // the ones that say whether the capture is whole, so a path that reached the
    // rubric without this table would report a clean bill of health for the
    // exact reason the rubric was extended to stop doing that.
    mount_capture_integrity(con, dir);
    return present;
}

}  // namespace pcomm::duck
