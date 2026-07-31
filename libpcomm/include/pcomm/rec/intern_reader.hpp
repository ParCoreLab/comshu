#pragma once
// L1 — resolving Handles back to what they stand for.
//
// Every high-cardinality value in a record — a pc, a pointer, a file path, a
// kernel name — is stored as a dense `Handle`, so records stay fixed-stride and
// the writer stays a memcpy. The dictionary that gives those handles meaning is
// therefore not optional context: without it, a capture is a pile of integers.
//
// It used to be `intern-<domain>.tsv`, bare text with no magic, no version and
// no schema, read by a lookup that returned "" for anything it could not find.
// A truncated, missing or mismatched dictionary produced empty cells and no
// error — indistinguishable from a capture that genuinely had no strings, while
// silently voiding every record in it. Every one of those cases is now reported.
//
// The index is `InternString` / `InternKey` records (framed, schema'd,
// conformance-checked like any other stream) and the string bytes live in
// `intern-blob.bin` behind a BlobHeader. Strings are the one thing that cannot
// sit in a record: a record is a fixed-stride array element.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/rec/stream_reader.hpp"

namespace pcomm::rec {

class InternDictionary {
public:
    // Handles restart at 1 in every domain, so handle 1 in String and handle 1
    // in KernelName are different things. The old layout hid that by keeping one
    // file per domain and only ever loading one of them.
    static std::uint64_t key(InternDomain d, core::Handle h) {
        return (static_cast<std::uint64_t>(d) << 32) | h;
    }

    const char* str(InternDomain d, core::Handle h) const {
        auto it = m_strings.find(key(d, h));
        return it == m_strings.end() ? "" : it->second.c_str();
    }
    const char* str(core::Handle h) const { return str(InternDomain::String, h); }

    // The raw value behind a numeric handle (a pc, a pointer). Returns false
    // rather than a plausible zero when the handle is unknown.
    bool value(InternDomain d, core::Handle h, std::uint64_t& out) const {
        auto it = m_keys.find(key(d, h));
        if (it == m_keys.end()) return false;
        out = it->second;
        return true;
    }

    std::size_t string_count() const { return m_strings.size(); }
    std::size_t key_count() const { return m_keys.size(); }

    // Loads one rank's dictionary. Diagnostics go to `diag`; the returned
    // dictionary is whatever could be resolved, so a partial failure still
    // exports the rest of the capture — but never quietly.
    static InternDictionary load(const std::filesystem::path& rank_dir,
                                 StreamDiagnostic diag = warn_to_stderr) {
        namespace fs = std::filesystem;
        InternDictionary dict;

        for (const auto& k : load_stream<core::records::InternKey>(
                 stream_path<core::records::InternKey>(rank_dir), diag))
            dict.m_keys.emplace(key(static_cast<InternDomain>(k.payload.domain),
                                    k.payload.handle), k.payload.key);

        const auto index = load_stream<core::records::InternString>(
            stream_path<core::records::InternString>(rank_dir), diag);
        if (index.empty()) return dict;

        const auto blob_path = rank_dir / "intern-blob.bin";
        std::error_code ec;
        const auto size = fs::file_size(blob_path, ec);
        if (ec) {
            diag(blob_path, std::to_string(index.size()) +
                     " interned strings are indexed but intern-blob.bin is missing — "
                     "every string in this rank is unresolvable");
            return dict;
        }

        std::ifstream in(blob_path, std::ios::binary);
        BlobHeader bh{};
        if (size < sizeof bh || !in.read(reinterpret_cast<char*>(&bh), sizeof bh)) {
            diag(blob_path, "too short to hold a blob header");
            return dict;
        }
        if (const char* err = blob_header_error(bh, size - sizeof bh)) {
            diag(blob_path, err);
            return dict;
        }
        if (bh.schema_hash != kSchemaHash) {
            char buf[128];
            std::snprintf(buf, sizeof buf,
                          "written by schema %016llx, this build has %016llx",
                          (unsigned long long)bh.schema_hash,
                          (unsigned long long)kSchemaHash);
            diag(blob_path, buf);
        }

        std::string blob(static_cast<std::size_t>(bh.byte_count), '\0');
        in.read(blob.data(), static_cast<std::streamsize>(blob.size()));

        std::size_t out_of_range = 0;
        for (const auto& s : index) {
            const std::uint64_t off = s.payload.offset;
            const std::uint64_t len = s.payload.length;
            if (off + len > blob.size()) { ++out_of_range; continue; }
            dict.m_strings.emplace(
                key(static_cast<InternDomain>(s.payload.domain), s.payload.handle),
                blob.substr(static_cast<std::size_t>(off), static_cast<std::size_t>(len)));
        }
        if (out_of_range)
            diag(blob_path, std::to_string(out_of_range) +
                     " intern entries point past the end of the blob — the index and "
                     "the blob are from different runs");
        return dict;
    }

private:
    std::unordered_map<std::uint64_t, std::string>   m_strings;
    std::unordered_map<std::uint64_t, std::uint64_t> m_keys;
};

}  // namespace pcomm::rec
