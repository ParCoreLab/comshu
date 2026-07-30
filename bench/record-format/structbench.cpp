// FlatBuffers STRUCT vs raw POD — the comparison that matters, since SigmaTrace's
// Simple.fbs uses `struct`, not `table`. FB structs are fixed-layout and inline:
// no vtable, no offsets. The earlier bench.cpp measured TABLES and is not the
// right comparison for this proposal.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "gpuaccess_generated.h"
#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

using namespace pcomm;
using Rec   = rec::Stamped<core::records::GpuAccess>;
using FBRec = pcomm::fb::GpuAccessF;
using Clock = std::chrono::steady_clock;
using Ns    = std::chrono::duration<double, std::nano>;
static constexpr std::size_t kN = 3'200'000, kBatch = 4096;

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::printf("sizeof(Stamped<GpuAccess]) = %zu   sizeof(GpuAccessF) = %zu\n\n",
                sizeof(Rec), sizeof(FBRec));
    double ta = 0, tc = 0; std::size_t ba = 0, bc = 0;

    {   // A: raw POD, current path
        std::vector<Rec> arena(kBatch);
        std::FILE* f = std::fopen((dir + "/sb_a.pcs").c_str(), "wb");
        std::size_t n = 0; auto t0 = Clock::now();
        for (std::size_t i = 0; i < kN; ++i) {
            Rec& r = arena[n++];
            r.temporal        = core::Temporal::instant({0, 1'000'000ull + i});
            r.payload.pc      = static_cast<core::Handle>(i & 0xffff);
            r.payload.object  = static_cast<core::Handle>(i & 0xff);
            r.payload.address = 0x7f0000000000ull + i * 128;
            r.payload.size    = 4;
            r.payload.op      = core::MemOp::Load;
            r.payload.space   = core::MemSpace::Global;
            r.payload.thread  = static_cast<std::uint32_t>(i & 0x3ff);
            if (n == kBatch) { ba += std::fwrite(arena.data(), 1, n*sizeof(Rec), f); n = 0; }
        }
        if (n) ba += std::fwrite(arena.data(), 1, n*sizeof(Rec), f);
        std::fflush(f); ta = std::chrono::duration_cast<Ns>(Clock::now()-t0).count();
        std::fclose(f);
    }
    {   // C: FlatBuffers struct, constructed in place in the same arena shape
        std::vector<FBRec> arena(kBatch);
        std::FILE* f = std::fopen((dir + "/sb_c.fb").c_str(), "wb");
        std::size_t n = 0; auto t0 = Clock::now();
        for (std::size_t i = 0; i < kN; ++i) {
            arena[n++] = FBRec(
                pcomm::fb::TemporalF(1,
                    pcomm::fb::TimePointF(0, 1'000'000ull + i),
                    pcomm::fb::TimePointF(0, 0), 0),
                static_cast<std::uint32_t>(i & 0xffff),
                static_cast<std::uint32_t>(i & 0xff),
                0x7f0000000000ull + i * 128,
                4, 0, 0, 0, 0,
                static_cast<std::uint32_t>(i & 0x3ff));
            if (n == kBatch) { bc += std::fwrite(arena.data(), 1, n*sizeof(FBRec), f); n = 0; }
        }
        if (n) bc += std::fwrite(arena.data(), 1, n*sizeof(FBRec), f);
        std::fflush(f); tc = std::chrono::duration_cast<Ns>(Clock::now()-t0).count();
        std::fclose(f);
    }
    std::printf("  A  raw POD (current)      %6.2f ns/rec   %7.2f MB   1.00x\n",
                ta/kN, ba/1048576.0);
    std::printf("  C  FlatBuffers STRUCT     %6.2f ns/rec   %7.2f MB   %4.2fx\n",
                tc/kN, bc/1048576.0, (tc/kN)/(ta/kN));
    return 0;
}
