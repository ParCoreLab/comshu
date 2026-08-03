#pragma once
// L1 — reading a record family across schema versions.
//
// A record's layout is frozen the moment a capture holding it exists, so
// evolving one means a new type_id at the next version rather than an edit (see
// PCOMM_RECORD_V in core/record.hpp). That is only half a mechanism: it says
// how to *write* a v2, and says nothing about a reader faced with a directory
// of captures, some written before the bump and some after.
//
// This is the other half. `load_family` reads every version present, converts
// the old ones forward through an upgrade function per step, and hands back one
// vector of the newest type. A capture from before the bump and one from after
// produce the same table, so goldens and queries are written against the family
// name — which is stable — and never against a version.
//
// Deliberately a chain of single-step upgrades (v1->v2, v2->v3) rather than a
// function per (old, newest) pair: the pair count grows quadratically and every
// one of them is a place to encode the same field twice and get it wrong once.
// Each step is written when that version is added, next to the record it knows
// about, and is never touched again.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pcomm/core/record.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

namespace pcomm::rec {

// What a reader has to say when a stream it cannot handle turns up. Silence is
// the one option not offered: an unreadable stream that produces no row and no
// message is indistinguishable from an empty one, which is exactly the class of
// failure the schema work exists to end.
struct FamilyDiagnostic {
    core::RecordTypeId type_id{0};
    std::string        message;
};

// Reads one version of a family and appends it, upgraded, to `out`.
//
// `Newest` is the current record type; `Old` is the version on disk. `Upgrade`
// maps one Old to one Newest — Stamped, because the temporal stamp carries
// forward untouched and a version bump must never silently restamp a record.
template<class Newest, class Old, class Loader, class Upgrade>
std::size_t load_version(const Loader& load, const Upgrade& upgrade,
                         std::vector<Stamped<Newest>>& out) {
    static_assert(core::RecordTrait<Old>::version <= core::RecordTrait<Newest>::version,
                  "upgrades run forward: Old must not be newer than Newest");
    static_assert(core::RecordTrait<Old>::name == core::RecordTrait<Newest>::name,
                  "a version bump keeps the family name — that name is the "
                  "stable identity every query is written against");
    static_assert(core::RecordTrait<Old>::kind == core::RecordTrait<Newest>::kind,
                  "a record does not change kind across versions");

    auto olds = load.template operator()<Old>();
    for (const auto& o : olds) {
        Stamped<Newest> n{};
        n.temporal = o.temporal;  // carried, never recomputed
        n.payload = upgrade(o.payload);
        out.push_back(n);
    }
    return olds.size();
}

// The identity step, for the newest version: it is already what it should be.
template<class Newest, class Loader>
std::size_t load_current(const Loader& load, std::vector<Stamped<Newest>>& out) {
    auto cur = load.template operator()<Newest>();
    out.insert(out.end(), cur.begin(), cur.end());
    return cur.size();
}

// Streams in a rank directory that this build has no record type for.
//
// The case that matters is a capture written by a *newer* build: it holds a
// version of a family this reader has never heard of, in a file whose name is
// just a number. Nothing about reading the rest of the directory fails, so
// without this the new stream contributes no rows and no message, and the
// export looks complete. That is the failure mode the whole schema effort
// exists to remove, arriving one level up — so a reader must be able to say
// "there is data here I cannot represent" out loud.
inline std::vector<FamilyDiagnostic>
unhandled_streams(const std::filesystem::path&           rank_dir,
                  const std::vector<core::RecordTypeId>& known) {
    namespace fs = std::filesystem;
    std::vector<FamilyDiagnostic> out;
    std::error_code               ec;
    for (const auto& entry : fs::directory_iterator(rank_dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pcs")
            continue;

        StreamHeader  h{};
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(&h), sizeof h) || h.magic != kStreamMagic) {
            out.push_back(
                {0, entry.path().filename().string() + ": not a format-1 stream — re-capture it"});
            continue;
        }
        if (std::find(known.begin(), known.end(), h.type_id) != known.end())
            continue;

        const auto payload = entry.file_size() - sizeof(StreamHeader);
        out.push_back({h.type_id, entry.path().filename().string() + ": type " +
                                      std::to_string(h.type_id) + " is unknown to this build (" +
                                      std::to_string(h.stride ? payload / h.stride : 0) +
                                      " records ignored) — a capture from a newer schema, or "
                                      "a provider this build does not know"});
    }
    return out;
}

}  // namespace pcomm::rec
