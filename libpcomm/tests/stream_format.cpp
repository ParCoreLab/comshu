// Round-trips the .pcs stream format: FileSink writes it, and the same
// validation pcomm-read uses reads it back.
//
// FileSink and StreamHeader are header-only, so this needs no engine and no
// provider — which matters, because it is the only thing that exercises the
// writer without the cluster. It covers what the format is *for*: a file that
// says what it holds, and a reader that refuses it when the answer is wrong
// instead of decoding it into plausible nonsense.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <unistd.h>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/sink.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

namespace fs = std::filesystem;
using namespace pcomm;
using core::records::MemObject;
using rec::Stamped;
using rec::StreamHeader;

namespace {

int g_fail = 0;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok " : "XX ", what);
    if (!ok)
        g_fail = 1;
}

// Submit `n` records of one type through the sink's actual interface.
void write_records(rec::ISink& sink, std::size_t n) {
    std::vector<Stamped<MemObject>> recs(n);
    for (std::size_t i = 0; i < n; ++i) {
        recs[i].temporal = core::Temporal::instant({1, i});
        recs[i].payload.id = static_cast<core::Handle>(i + 1);
        recs[i].payload.range = {i * 4096, 4096};
        recs[i].payload.device = 2;
    }
    rec::BufferView view{core::RecordTrait<MemObject>::type_id, core::RecordTrait<MemObject>::kind,
                         reinterpret_cast<const std::byte*>(recs.data()),
                         recs.size() * sizeof(Stamped<MemObject>), recs.size()};
    sink.submit(view);
}

StreamHeader read_header(const fs::path& p) {
    StreamHeader  h{};
    std::ifstream in(p, std::ios::binary);
    in.read(reinterpret_cast<char*>(&h), sizeof h);
    return h;
}

}  // namespace

int main() {
    std::printf("stream format round-trip\n");

    const auto dir =
        fs::temp_directory_path() / ("pcomm-stream-format-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const auto stream = dir / (std::to_string(core::RecordTrait<MemObject>::type_id) + ".pcs");
    constexpr std::size_t kFirst = 50, kSecond = 25;

    {
        rec::FileSink sink(dir);
        write_records(sink, kFirst);
    }

    expect(fs::exists(stream), "stream file created");
    expect(fs::file_size(stream) == sizeof(StreamHeader) + kFirst * sizeof(Stamped<MemObject>),
           "file is header + records, exactly");

    const auto h = read_header(stream);
    expect(h.magic == rec::kStreamMagic, "magic");
    expect(h.format_version == rec::kStreamFormatVersion, "format version");
    expect(h.type_id == core::RecordTrait<MemObject>::type_id, "type_id matches the record");
    expect(h.kind == core::RecordTrait<MemObject>::kind, "kind matches the record");
    expect(h.stride == sizeof(Stamped<MemObject>), "stride is sizeof(Stamped<R>)");
    expect(h.schema_hash == rec::kSchemaHash, "schema hash is this build's schema");
    expect(rec::stream_header_error(h, core::RecordTrait<MemObject>::type_id,
                                    sizeof(Stamped<MemObject>)) == nullptr,
           "validates against the expected type and stride");

    // The magic is the literal byte sequence, in file order — a reader in
    // another language matches on these bytes, not on our uint64 spelling.
    {
        unsigned char first8[8]{};
        std::ifstream in(stream, std::ios::binary);
        in.read(reinterpret_cast<char*>(first8), sizeof first8);
        const unsigned char want[8] = {0x89, 'P', 'C', 'O', 'M', 'M', 'S', 0x0A};
        expect(std::memcmp(first8, want, 8) == 0, "first 8 bytes are 89 P C O M M S \\n");
    }

    // Reopening an existing capture directory appends records; a second header
    // mid-file would be indistinguishable from a record.
    {
        rec::FileSink sink(dir);
        write_records(sink, kSecond);
    }
    expect(fs::file_size(stream) ==
               sizeof(StreamHeader) + (kFirst + kSecond) * sizeof(Stamped<MemObject>),
           "reopening appends records without a second header");

    // Records survive the round trip, read at the stride the *file* declares
    // rather than the one this build assumes.
    {
        std::ifstream in(stream, std::ios::binary);
        in.seekg(sizeof(StreamHeader));
        std::vector<Stamped<MemObject>> back(kFirst + kSecond);
        in.read(reinterpret_cast<char*>(back.data()),
                static_cast<std::streamsize>(back.size() * h.stride));
        bool ok = back[0].payload.id == 1 && back[kFirst - 1].payload.id == kFirst &&
                  back[kFirst].payload.id == 1 && back[kFirst].payload.range.base == 0 &&
                  back[kFirst + kSecond - 1].payload.id == kSecond;
        expect(ok, "records read back at the declared stride");
    }

    // And the checks refuse what they should. These are the cases that used to
    // be undetectable: before the header there was nothing to disagree with.
    {
        StreamHeader bad = h;
        bad.magic = 0;
        expect(rec::stream_header_error(bad, core::RecordTrait<MemObject>::type_id,
                                        sizeof(Stamped<MemObject>)) != nullptr,
               "refuses a headerless (pre-format-1) capture");

        bad = h;
        bad.format_version = 99;
        expect(rec::stream_header_error(bad, core::RecordTrait<MemObject>::type_id,
                                        sizeof(Stamped<MemObject>)) != nullptr,
               "refuses a future format version");

        bad = h;
        bad.type_id = 999;
        expect(rec::stream_header_error(bad, core::RecordTrait<MemObject>::type_id,
                                        sizeof(Stamped<MemObject>)) != nullptr,
               "refuses a stream whose type is not what was asked for");

        bad = h;
        bad.stride = h.stride + 8;
        expect(rec::stream_header_error(bad, core::RecordTrait<MemObject>::type_id,
                                        sizeof(Stamped<MemObject>)) != nullptr,
               "refuses a capture whose record layout differs from this build");
    }

    // An empty view must not leave a file framed with a stride of zero.
    {
        const auto      empty_dir = dir / "empty";
        rec::FileSink   sink(empty_dir);
        rec::BufferView view{core::RecordTrait<MemObject>::type_id,
                             core::RecordTrait<MemObject>::kind, nullptr, 0, 0};
        sink.submit(view);
        expect(!fs::exists(empty_dir / "520.pcs"),
               "an empty buffer writes no file rather than a strideless one");
    }

    fs::remove_all(dir);
    std::printf("%s\n",
                g_fail ? "FAIL" : "PASS — .pcs format round-trips and rejects what it should");
    return g_fail;
}
