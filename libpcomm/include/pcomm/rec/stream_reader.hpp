#pragma once
// L1 — reading one `.pcs` stream: validate the header, then map the rest as an
// array of Stamped<R>.
//
// This lived inside pcomm-read/main.cpp, where nothing could test it. It is the
// counterpart of FileSink and the only place the on-disk framing is interpreted,
// so it belongs next to the writer and in reach of a test — particularly now
// that record versions mean a reader has to decide what to do about a stream it
// was not built for.
//
// The header is validated, never skipped. A capture that disagrees about the
// record type or the stride is refused by name rather than decoded into
// plausible nonsense, which is what happened for as long as the files carried no
// framing at all.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pcomm/core/record.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

namespace pcomm::rec {

// Where a stream that cannot be read says so. Defaulted to stderr because the
// one behaviour that must not exist is silence: a refused stream that produces
// no rows and no message is indistinguishable from an empty one.
using StreamDiagnostic = void (*)(const std::filesystem::path&, const std::string&);

inline void warn_to_stderr(const std::filesystem::path& p, const std::string& msg) {
    std::fprintf(stderr, "pcomm: %s: %s\n", p.c_str(), msg.c_str());
}

// What load_stream found, for a caller that has to REPORT damage rather than
// merely warn about it.
//
// The diagnostic callback above is the older half and it is not enough: it hands
// out prose, so the only way to learn "this stream was unreadable" from it is to
// match on the wording of an error message. This says it in fields. `pcomm
// export` fills one of these per stream and turns them into the
// `capture_integrity` table, which is how a reader who has ONLY the Parquet
// finds out that a file was zeroed or cut short — see link_health's
// capture_streams_unreadable and capture_streams_truncated.
struct StreamLoad {
    bool unreadable{false};  // present but refused: no header, wrong type, bad stride
    bool truncated{false};   // tail is a partial record — a whole one is missing
    bool foreign_schema{false};
    std::uint64_t records{0};
};

// The conventional path of one record type's stream inside a rank directory.
template<core::Record R> std::filesystem::path stream_path(const std::filesystem::path& rank_dir) {
    return rank_dir / (std::to_string(core::RecordTrait<R>::type_id) + ".pcs");
}

// Reads every record in one stream. A missing file is not an error — a run that
// never emitted a record type simply has no file for it.
template<core::Record R>
std::vector<Stamped<R>> load_stream(const std::filesystem::path& file,
                                    StreamDiagnostic             diag = warn_to_stderr,
                                    StreamLoad*                  found = nullptr) {
    namespace fs = std::filesystem;
    std::vector<Stamped<R>> out;
    std::error_code         ec;
    if (!fs::exists(file, ec) || ec)
        return out;

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        diag(file, "cannot be opened");
        if (found)
            found->unreadable = true;
        return out;
    }

    // THE ERROR_CODE OVERLOAD, and not for tidiness. The throwing one terminated
    // `pcomm export` outright: `terminate called after throwing an instance of
    // std::filesystem::filesystem_error ... cannot get file size: Is a
    // directory`. Found 2026-09-03 while testing what a damaged capture does to
    // the reader — which is the whole point of the surrounding work, and the
    // reader crashing on a damaged capture is the same defect class as the
    // reader lying about one.
    //
    // A directory named `590.pcs` is contrived; the gap between exists() above
    // and this line is not. A capture being copied off a cluster while it is
    // read loses files underneath the reader, and that raced path threw too.
    const auto bytes = fs::file_size(file, ec);
    if (ec) {
        diag(file, "cannot be sized — not a regular file, or it vanished mid-read");
        if (found)
            found->unreadable = true;
        return out;
    }
    if (bytes < sizeof(StreamHeader)) {
        diag(file, "too short to hold a stream header");
        if (found)
            found->unreadable = true;
        return out;
    }

    StreamHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof h);
    if (const char* err =
            stream_header_error(h, core::RecordTrait<R>::type_id, sizeof(Stamped<R>),
                                core::RecordTrait<R>::kind)) {
        diag(file, err);
        if (found)
            found->unreadable = true;
        return out;
    }
    // A different schema wrote this, but the type and stride still agree, so the
    // records decode. Say so rather than either failing or staying quiet: an
    // unrelated record changing is not a reason to refuse this stream.
    if (h.schema_hash != kSchemaHash) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "written by schema %016llx, this build has %016llx — layout "
                      "agrees, decoding anyway",
                      (unsigned long long)h.schema_hash, (unsigned long long)kSchemaHash);
        diag(file, buf);
        if (found)
            found->foreign_schema = true;
    }

    const auto payload = bytes - sizeof(StreamHeader);
    if (payload % h.stride) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "%llu bytes of records is not a whole number of %u-byte "
                      "records — truncated capture, reading what is complete",
                      (unsigned long long)payload, h.stride);
        diag(file, buf);
        if (found)
            found->truncated = true;
    }
    out.resize(payload / sizeof(Stamped<R>));
    in.read(reinterpret_cast<char*>(out.data()),
            static_cast<std::streamsize>(out.size() * sizeof(Stamped<R>)));
    if (found)
        found->records = out.size();
    return out;
}

}  // namespace pcomm::rec
