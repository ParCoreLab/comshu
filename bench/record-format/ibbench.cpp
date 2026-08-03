// The ibverbs question: post_send is on the application's critical path, so the
// record-append cost is a direct tax on every traced call. Same comparison as
// bench.cpp but with the REAL IbPostSend record (Stamped = 88 bytes).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "flatbuffers/flatbuffers.h"
#include "ib_records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

using namespace pcomm;
using PS = providers::ibverbs::records::IbPostSend;
using Rec = rec::Stamped<PS>;
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::duration<double, std::nano>;

static constexpr std::size_t kN = 2'000'000;
static constexpr std::size_t kBatch = 4096;

static Rec make(std::size_t i) {
    Rec r{};
    // post_send is hooked with a real interval [call-start, call-end], unlike
    // the GpuAccess instants — so temporal.b is populated here and FlatBuffers
    // cannot elide it as a default. That is the realistic case.
    r.temporal = core::Temporal::interval({0, 1000ull + i * 400}, {0, 1180ull + i * 400});
    r.payload.qp = static_cast<core::Handle>(i & 0xff);
    r.payload.wr_id = i;
    r.payload.remote_addr = 0;  // SEND: not an RDMA op
    r.payload.rkey = 0;
    r.payload.length = 4096;
    r.payload.qp_num = 0x1effd;
    r.payload.opcode = 0;
    r.payload.num_sge = 1;
    return r;
}

static flatbuffers::Offset<void> build(flatbuffers::FlatBufferBuilder& b, const Rec& r) {
    const auto s = b.StartTable();
    b.AddElement<std::uint64_t>(4, r.temporal.a.ticks, 0);
    b.AddElement<std::uint64_t>(6, r.temporal.b.ticks, 0);
    b.AddElement<std::uint8_t>(8, static_cast<std::uint8_t>(r.temporal.kind), 0);
    b.AddElement<std::uint32_t>(10, r.payload.qp, 0);
    b.AddElement<std::uint64_t>(12, r.payload.wr_id, 0);
    b.AddElement<std::uint64_t>(14, r.payload.remote_addr, 0);
    b.AddElement<std::uint32_t>(16, r.payload.rkey, 0);
    b.AddElement<std::uint32_t>(18, r.payload.length, 0);
    b.AddElement<std::uint32_t>(20, r.payload.qp_num, 0);
    b.AddElement<std::uint8_t>(22, r.payload.opcode, 0);
    b.AddElement<std::uint8_t>(24, r.payload.num_sge, 0);
    return flatbuffers::Offset<void>(b.EndTable(s));
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::printf("ibverbs record: sizeof(Stamped<IbPostSend]) = %zu\n\n", sizeof(Rec));

    // A: raw POD append (current)
    {
        std::vector<Rec> arena(kBatch);
        std::FILE*       f = std::fopen((dir + "/ib_a.pcs").c_str(), "wb");
        std::size_t      n = 0, bytes = 0;
        auto             t0 = Clock::now();
        for (std::size_t i = 0; i < kN; ++i) {
            arena[n++] = make(i);
            if (n == kBatch) {
                bytes += std::fwrite(arena.data(), 1, n * sizeof(Rec), f);
                n = 0;
            }
        }
        if (n)
            bytes += std::fwrite(arena.data(), 1, n * sizeof(Rec), f);
        std::fflush(f);
        auto dt = std::chrono::duration_cast<Ns>(Clock::now() - t0).count();
        std::fclose(f);
        std::printf("  A  raw POD append        %6.2f ns/rec   %7.2f MB\n", dt / double(kN),
                    bytes / 1048576.0);
    }
    // B2: FlatBuffers batched
    {
        std::FILE*                             f = std::fopen((dir + "/ib_b2.fb").c_str(), "wb");
        std::vector<flatbuffers::Offset<void>> offs;
        offs.reserve(kBatch);
        flatbuffers::FlatBufferBuilder b(kBatch * 96);
        std::size_t                    bytes = 0;
        auto                           t0 = Clock::now();
        auto                           flush = [&] {
            auto vec = b.CreateVector(
                reinterpret_cast<const flatbuffers::Offset<flatbuffers::Table>*>(offs.data()),
                offs.size());
            const auto s = b.StartTable();
            b.AddOffset(4, vec);
            auto root = b.EndTable(s);
            b.FinishSizePrefixed(flatbuffers::Offset<flatbuffers::Table>(root));
            bytes += std::fwrite(b.GetBufferPointer(), 1, b.GetSize(), f);
            b.Clear();
            offs.clear();
        };
        for (std::size_t i = 0; i < kN; ++i) {
            offs.push_back(build(b, make(i)));
            if (offs.size() == kBatch)
                flush();
        }
        if (!offs.empty())
            flush();
        std::fflush(f);
        auto dt = std::chrono::duration_cast<Ns>(Clock::now() - t0).count();
        std::fclose(f);
        std::printf("  B2 FlatBuffers batched   %6.2f ns/rec   %7.2f MB\n", dt / double(kN),
                    bytes / 1048576.0);
    }
    return 0;
}
