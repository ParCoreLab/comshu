// Record-format benchmark: raw-POD .pcs vs FlatBuffers.
//
// Uses the REAL record types (pcomm::core::records::GpuAccess, the firehose)
// and the real Stamped<R> wrapper, so the numbers describe pcomm rather than a
// synthetic stand-in. sizeof(Stamped<GpuAccess>) == 80.
//
// Paths measured:
//   A  raw POD, current path  — append into a 4096-slot arena, fwrite on flush
//   A' raw POD + file header  — the proposed self-description fix
//   B1 FlatBuffers, streaming — one size-prefixed table per record
//   B2 FlatBuffers, batched   — one vector<table> per 4096-record flush, which
//                               is the fair comparison since path A also batches
//
// FlatBuffers tables are built with the FlatBufferBuilder API directly rather
// than flatc-generated code. That is the same call sequence generated code
// emits (StartTable / AddElement / EndTable) and the same wire format, so it
// does not flatter either side.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"

#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

using namespace pcomm;
using GA = core::records::GpuAccess;
using Rec = rec::Stamped<GA>;
using Clock = std::chrono::steady_clock;
using NsCount = std::chrono::duration<double, std::nano>;

static constexpr std::size_t kRecords = 3'200'000;  // case 05 scale, per rank
static constexpr std::size_t kBatch = 4096;         // ThreadLocalBuffer capacity

// Deterministic pseudo-data so every path serializes identical values.
static Rec make_record(std::size_t i) {
    Rec r{};
    r.temporal = core::Temporal::instant({0, 1'000'000ull + i});
    r.payload.pc = static_cast<core::Handle>(i & 0xffff);
    r.payload.object = static_cast<core::Handle>(i & 0xff);
    r.payload.address = 0x7f0000000000ull + (i * 128);
    r.payload.size = 4;
    r.payload.op = core::MemOp::Load;
    r.payload.space = core::MemSpace::Global;
    r.payload.thread = static_cast<std::uint32_t>(i & 0x3ff);
    return r;
}

struct Result {
    std::string name;
    double      ns_per_record;
    std::size_t bytes;
};

static void report(const Result& r, const Result& base) {
    std::printf("  %-28s %8.2f ns/rec  %10.2f MB  %6.2fx time  %5.2fx size\n", r.name.c_str(),
                r.ns_per_record, double(r.bytes) / (1024.0 * 1024.0),
                r.ns_per_record / base.ns_per_record, double(r.bytes) / double(base.bytes));
}

// --- A: raw POD, exactly what FileSink does today --------------------------
static Result bench_raw(const char* path, bool with_header) {
    std::vector<Rec> arena(kBatch);
    std::FILE*       f = std::fopen(path, "wb");
    std::size_t      bytes = 0;
    auto             t0 = Clock::now();
    if (with_header) {
        // magic, format version, type id, sizeof(Stamped<R>) — the fix that
        // turns a silent misparse into a loud error. Once per file.
        struct {
            char          magic[4];
            std::uint32_t ver, type_id, stride;
        } h{{'P', 'C', 'S', '1'}, 1u, 700u, static_cast<std::uint32_t>(sizeof(Rec))};
        bytes += std::fwrite(&h, 1, sizeof h, f);
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < kRecords; ++i) {
        arena[n++] = make_record(i);  // the append: a POD store
        if (n == kBatch) {            // the flush: one fwrite
            bytes += std::fwrite(arena.data(), 1, n * sizeof(Rec), f);
            n = 0;
        }
    }
    if (n)
        bytes += std::fwrite(arena.data(), 1, n * sizeof(Rec), f);
    std::fflush(f);
    auto dt = std::chrono::duration_cast<NsCount>(Clock::now() - t0).count();
    std::fclose(f);
    return {with_header ? "A' raw POD + header" : "A  raw POD (current)", dt / double(kRecords),
            bytes};
}

// Build one GpuAccess table. Field slots mirror what flatc would emit.
static flatbuffers::Offset<void> build_one(flatbuffers::FlatBufferBuilder& b, const Rec& r) {
    const auto start = b.StartTable();
    b.AddElement<std::uint64_t>(4, r.temporal.a.ticks, 0);
    b.AddElement<std::uint64_t>(6, r.temporal.b.ticks, 0);
    b.AddElement<std::uint64_t>(8, r.temporal.value, 0);
    b.AddElement<std::uint8_t>(10, static_cast<std::uint8_t>(r.temporal.kind), 0);
    b.AddElement<std::uint32_t>(12, r.payload.pc, 0);
    b.AddElement<std::uint32_t>(14, r.payload.object, 0);
    b.AddElement<std::uint64_t>(16, r.payload.address, 0);
    b.AddElement<std::uint32_t>(18, r.payload.size, 0);
    b.AddElement<std::uint8_t>(20, static_cast<std::uint8_t>(r.payload.op), 0);
    b.AddElement<std::uint8_t>(22, static_cast<std::uint8_t>(r.payload.space), 0);
    b.AddElement<std::uint32_t>(24, r.payload.thread, 0);
    return flatbuffers::Offset<void>(b.EndTable(start));
}

// --- B1: FlatBuffers, one size-prefixed message per record -----------------
static Result bench_fb_stream(const char* path) {
    std::FILE*  f = std::fopen(path, "wb");
    std::size_t bytes = 0;
    auto        t0 = Clock::now();
    for (std::size_t i = 0; i < kRecords; ++i) {
        flatbuffers::FlatBufferBuilder b(256);
        auto                           off = build_one(b, make_record(i));
        b.FinishSizePrefixed(flatbuffers::Offset<flatbuffers::Table>(off.o));
        bytes += std::fwrite(b.GetBufferPointer(), 1, b.GetSize(), f);
    }
    std::fflush(f);
    auto dt = std::chrono::duration_cast<NsCount>(Clock::now() - t0).count();
    std::fclose(f);
    return {"B1 FlatBuffers streaming", dt / double(kRecords), bytes};
}

// --- B2: FlatBuffers, one vector of tables per 4096-record flush ------------
static Result bench_fb_batched(const char* path) {
    std::FILE*                             f = std::fopen(path, "wb");
    std::size_t                            bytes = 0;
    auto                                   t0 = Clock::now();
    std::vector<flatbuffers::Offset<void>> offs;
    offs.reserve(kBatch);
    flatbuffers::FlatBufferBuilder b(kBatch * 96);
    for (std::size_t i = 0; i < kRecords; ++i) {
        offs.push_back(build_one(b, make_record(i)));
        if (offs.size() == kBatch) {
            auto vec = b.CreateVector(
                reinterpret_cast<const flatbuffers::Offset<flatbuffers::Table>*>(offs.data()),
                offs.size());
            const auto start = b.StartTable();
            b.AddOffset(4, vec);
            auto root = b.EndTable(start);
            b.FinishSizePrefixed(flatbuffers::Offset<flatbuffers::Table>(root));
            bytes += std::fwrite(b.GetBufferPointer(), 1, b.GetSize(), f);
            b.Clear();
            offs.clear();
        }
    }
    if (!offs.empty()) {  // the tail: 3200000 % 4096 == 1024 records
        auto vec = b.CreateVector(
            reinterpret_cast<const flatbuffers::Offset<flatbuffers::Table>*>(offs.data()),
            offs.size());
        const auto start = b.StartTable();
        b.AddOffset(4, vec);
        auto root = b.EndTable(start);
        b.FinishSizePrefixed(flatbuffers::Offset<flatbuffers::Table>(root));
        bytes += std::fwrite(b.GetBufferPointer(), 1, b.GetSize(), f);
    }
    std::fflush(f);
    auto dt = std::chrono::duration_cast<NsCount>(Clock::now() - t0).count();
    std::fclose(f);
    return {"B2 FlatBuffers batched", dt / double(kRecords), bytes};
}

// --- read back: the analysis path (pcomm-read --csv) ----------------------
// Touch every record and accumulate a field, so the compiler cannot elide the
// parse and so the numbers include whatever pointer-chasing each format needs.
static double read_raw(const char* path, std::uint64_t& sink) {
    std::FILE* f = std::fopen(path, "rb");
    std::fseek(f, 0, SEEK_END);
    long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<Rec> v(bytes / sizeof(Rec));
    if (std::fread(v.data(), 1, v.size() * sizeof(Rec), f) == 0) {
    }
    auto t0 = Clock::now();  // load OUTSIDE the timer, same as the FB path
    for (const auto& r : v)
        sink += r.payload.address;  // zero-copy: already the struct
    auto dt = std::chrono::duration_cast<NsCount>(Clock::now() - t0).count();
    std::fclose(f);
    return dt / double(v.size());
}

static double read_fb_batched(const char* path, std::uint64_t& sink, std::size_t& n) {
    std::FILE* f = std::fopen(path, "rb");
    std::fseek(f, 0, SEEK_END);
    long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> buf(bytes);
    if (std::fread(buf.data(), 1, bytes, f) == 0) {
    }
    std::fclose(f);
    n = 0;
    auto        t0 = Clock::now();
    std::size_t off = 0;
    while (off + 4 <= buf.size()) {
        std::uint32_t       sz = flatbuffers::ReadScalar<std::uint32_t>(buf.data() + off);
        const std::uint8_t* msg = buf.data() + off + 4;
        auto*               root = flatbuffers::GetRoot<flatbuffers::Table>(msg);
        auto*               vec =
            root->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
                4);
        if (vec) {
            for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
                const auto* t = vec->Get(i);
                sink += t->GetField<std::uint64_t>(16, 0);
                ++n;
            }
        }
        off += 4 + sz;
    }
    auto dt = std::chrono::duration_cast<NsCount>(Clock::now() - t0).count();
    return dt / double(n ? n : 1);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::printf("pcomm record-format benchmark\n");
    std::printf("  records: %zu (case 05 scale, per rank)   batch: %zu\n", kRecords, kBatch);
    std::printf("  sizeof(Stamped<GpuAccess]) = %zu  (Temporal %zu + payload %zu)\n\n", sizeof(Rec),
                sizeof(core::Temporal), sizeof(GA));

    auto a = bench_raw((dir + "/bench_a.pcs").c_str(), false);
    auto ah = bench_raw((dir + "/bench_ah.pcs").c_str(), true);
    auto b1 = bench_fb_stream((dir + "/bench_b1.fb").c_str());
    auto b2 = bench_fb_batched((dir + "/bench_b2.fb").c_str());

    std::printf("WRITE PATH (lower is better; baseline = current raw POD)\n");
    report(a, a);
    report(ah, a);
    report(b1, a);
    report(b2, a);

    std::printf("\nREAD PATH (parse + touch every record)\n");
    std::uint64_t sink = 0;
    std::size_t   nfb = 0;
    double        ra = read_raw((dir + "/bench_a.pcs").c_str(), sink);
    double        rb = read_fb_batched((dir + "/bench_b2.fb").c_str(), sink, nfb);
    std::printf("  %-28s %8.2f ns/rec\n", "A  raw POD (zero-copy)", ra);
    std::printf("  %-28s %8.2f ns/rec  (%zu records, %.2fx)\n", "B2 FlatBuffers batched", rb, nfb,
                rb / ra);
    std::printf("  [checksum %llu]\n", (unsigned long long)sink);
    return 0;
}
