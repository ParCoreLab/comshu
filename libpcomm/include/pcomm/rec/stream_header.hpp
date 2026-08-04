#pragma once
// L1 — the first 32 bytes of every `.pcs` file.
//
// Before this existed a capture was a bare array of records. Which record type
// a file held came from its *filename*; which schema wrote it came from
// nowhere. A capture written before a schema change was byte-indistinguishable
// from one written after, so a reader that guessed wrong misparsed silently —
// there was no magic to check, no stride to disagree with, and no error to
// raise. That is the defect this closes, and it is the one that started the
// whole record-format investigation.
//
// The header is written once when a stream file is created, never per record,
// so it costs nothing on the hot path. 32 bytes keeps the records that follow
// 8-byte aligned, so `load_stream` can still map the remainder as an array.
//
// Declared in libpcomm/schema/pcomm.fbs as `StreamHeader` and byte-checked
// against it by tests/schema_conformance, exactly like the records.

#include <cstdint>

#include "pcomm/core/ids.hpp"

namespace pcomm::rec {

// The bytes 89 'P' 'C' 'O' 'M' 'M' 'S' 0A, in that order on disk. Read back as
// a little-endian uint64 this is the constant below; on a big-endian reader, or
// through anything that mangles a byte (a text-mode copy turning 0A into 0D 0A
// is the classic), it is not — which is the point of the high bit and the
// trailing newline. Same trick PNG uses.
inline constexpr std::uint64_t kStreamMagic = 0x0A534D4D4F435089ull;

// Bumped when the *framing* changes — a new field here, a different header
// size. Not when a record changes: that is what schema_hash and type_id are
// for.
inline constexpr std::uint16_t kStreamFormatVersion = 1;

// FNV-1a over libpcomm/schema/pcomm.fbs with comments stripped and whitespace
// collapsed, so prose edits do not churn it but any declaration change does.
// tools/schema_check.py recomputes this and fails the build when it drifts —
// keeping it correct is not left to whoever edits the schema remembering to.
inline constexpr std::uint64_t kSchemaHash = 0x44C2C114B026374Cull;

// Layout is fixed by pcomm.fbs. The padding is explicit and zero-initialised
// for the same reason core/time.hpp spells its padding out: an implicit hole
// puts uninitialised stack bytes on disk, and this one would land in the first
// 32 bytes of every capture file we ship.
struct StreamHeader {
    std::uint64_t      magic{kStreamMagic};
    std::uint16_t      format_version{kStreamFormatVersion};
    core::RecordTypeId type_id{0};
    std::uint32_t      stride{0};  // bytes per record: sizeof(Stamped<R>)
    std::uint64_t      schema_hash{kSchemaHash};
    core::RecordKind   kind{core::RecordKind::Event};
    std::uint8_t       _pad[7]{};  // keep: see above

    StreamHeader() = default;
    StreamHeader(core::RecordTypeId id, core::RecordKind k, std::uint32_t bytes_per_record)
        : type_id(id), stride(bytes_per_record), kind(k), _pad{} {}
};

static_assert(sizeof(StreamHeader) == 32);
static_assert(alignof(StreamHeader) == 8);

// The bytes 89 'P' 'C' 'B' 'L' 'O' 'B' 0A, by the same reasoning as
// kStreamMagic. A different constant because pairing a blob with the wrong
// capture must be detectable, and "it is a pcomm file" is not enough to know.
inline constexpr std::uint64_t kBlobMagic = 0x0A424F4C42435089ull;

// The header of `intern-blob.bin`, the byte heap interned strings point into.
//
// The blob is the one piece of a capture that is pure payload — offsets and
// lengths live in InternString records, and the bytes themselves have no
// internal structure at all. That makes a mismatched blob uniquely dangerous:
// every string in the capture resolves to the wrong text, at plausible offsets,
// with nothing out of place for a reader to notice. Hence a header, and hence a
// byte_count, so truncation is arithmetic rather than a guess.
struct BlobHeader {
    std::uint64_t magic{kBlobMagic};
    std::uint16_t format_version{kStreamFormatVersion};
    std::uint8_t  _pad[6]{};      // keep: explicit, see above
    std::uint64_t byte_count{0};  // payload bytes following this header
    std::uint64_t schema_hash{kSchemaHash};
};

static_assert(sizeof(BlobHeader) == 32);
static_assert(alignof(BlobHeader) == 8);

inline const char* blob_header_error(const BlobHeader& h, std::uint64_t actual_bytes) {
    if (h.magic != kBlobMagic)
        return "not a pcomm intern blob (bad magic)";
    if (h.format_version != kStreamFormatVersion)
        return "unsupported intern-blob format version";
    if (h.byte_count != actual_bytes)
        return "intern blob is truncated or padded — byte_count disagrees with "
               "the file size, so every string offset past the cut is wrong";
    return nullptr;
}

// Why a header is not enough on its own, and what a reader must still do:
// the fields say what the file claims to be, and this says whether the claim
// is one this build can honour. Returns nullptr when it is.
inline const char* stream_header_error(const StreamHeader& h, core::RecordTypeId expected_type,
                                       std::uint32_t expected_stride) {
    if (h.magic != kStreamMagic)
        return "not a pcomm stream (bad magic) — a pre-format-1 capture has no "
               "header at all and must be re-captured";
    if (h.format_version != kStreamFormatVersion)
        return "unsupported .pcs format version";
    if (h.type_id != expected_type)
        return "stream holds a different record type than its filename says";
    if (h.stride != expected_stride)
        return "record stride differs — this capture was written by a build "
               "whose record layout does not match this one";
    return nullptr;
}

}  // namespace pcomm::rec
