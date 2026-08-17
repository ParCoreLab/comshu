// libcupti_trace.so — kernel execution windows, from CUPTI's activity API.
//
// A backend rather than part of the engine, for the reason stated at the top of
// libpcomm/src/engine.cpp: the engine is L0-L2 core and nothing there may pull
// in a vendor header. The RECORD is still the engine's (GpuKernel, 530, in its
// own 500-599 range) and arrives through pcomm_engine_record_kernel; this file
// owns only the vendor binding. Same split as gpu-backends/nvbit.
//
// WHY THIS IS NOT SIMPLY ALWAYS ON — the measurement that shapes the whole file
// (tests/hpc/probes/kerneltime, MN5 job 44546506):
//
//   NVBit dlopens libcupti and takes the process's ONE CUPTI subscriber slot.
//   Whoever gets there first wins, and the failure is asymmetric:
//
//     CUPTI first  -> NVBit dies, CUDA_ERROR_ALREADY_ACQUIRED, process exit 1
//     NVBit first  -> we get CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED
//                     and everything else keeps working
//
//   Arm E ran the real libgpumem_trace.so with a CUPTI shim in the chain: four
//   rank directories, ZERO .pcs files. Not a crash — a silently empty capture.
//
// So this file must never be the one that wins. The guards, in the order they
// actually protect:
//
//   1. THE REAL ONE: pcomm_engine_cupti_allowed(). The launcher knows the
//      provider list before anything loads and clears it when gpumem is
//      selected, so we never touch CUPTI in a process NVBit is in. This is
//      deterministic and does not depend on timing.
//
//      It was pcomm_engine_kernel_timing_wanted() until 2026-08-17, and that
//      is a cautionary tale rather than a rename: NVBit's own kernel stamp read
//      the same flag, so the guard protecting THIS file from CUPTI was also
//      switching off the instrument that replaces it. Every gpumem capture ever
//      taken lost its clock to it. A guard names the hazard it guards.
//   2. Best-effort lateness. We arm from cuInit-after-the-real-call (the driver
//      runs CUDA_INJECTION64_PATH tools from inside cuInit, so NVBit has
//      already taken the slot by then) or from the first launch. That removes
//      the LD_PRELOAD-order dependency for hand-built chains — but ONLY when
//      those doors are reachable, and they are not always: a statically linked
//      cudart can reach the driver through cuGetProcAddress, which defeats
//      symbol interposition entirely (measured during the IBGDA work). So the
//      constructor arms as a last resort, and PCOMM_KERNEL_TIMING_LAZY=1 turns
//      that last resort off for anyone hand-rolling a chain with NVBit in it.
//
// Guard 2 is a convenience. Guard 1 is the one that must be right.
//
// Cost, measured on a real 8-rank NCCL all-reduce over IB: +0.65 %, which is
// inside that workload's own ~5 % run-to-run variance. 208 bytes of activity
// per kernel, ~1050 kernels/s/rank.

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include <cuda.h>
#include <cuda_runtime.h>
#include <cupti.h>

#include "pcomm/core/ids.hpp"
#include "pcomm/helpers/timestamp.hpp"
#include "pcomm/tracer.hpp"
#include "pcomm/core/vocab.hpp"
#include "pcomm/engine_abi.h"

namespace {

const char* cupti_err(CUptiResult r);  // defined below; used by try_tsc_timebase

constexpr std::size_t kBufSize = 8u << 20;
constexpr std::size_t kBufAlign = 8;

std::atomic<bool>     g_on{false};
std::atomic<uint64_t> g_kernels{0};
std::atomic<uint64_t> g_dropped{0};
std::once_flag        g_once;
bool                  g_verbose = false;

uint64_t wall_ns() {
    timespec t{};
    clock_gettime(CLOCK_REALTIME, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1'000'000'000ull + t.tv_nsec;
}

// ── CUPTI's timebase, replaced by ours ──────────────────────────────────────
//
// Lifted from SigmaTrace (CUDATrace/CUPTI/CUPTITrace.cpp), which registers this
// before it enables anything. CUPTI will call it for the timestamps it stamps
// activity records with, so records arrive already in TSC ticks — the SAME
// domain every host record in the capture uses.
//
// That deletes work rather than adding it. Without this, kernels land on
// domain 1, which needs its own ClockAnchor pair per rank and its own fit in
// `_clock`, and a capture that loses those anchors loses every kernel row to
// link_health.timeline_unanchored. With it, kernels are domain 0 and ride the
// anchors the engine already emits.
//
// IT IS NOT ASSUMED TO WORK. Two things could make it a lie: an older CUPTI
// without the entry point (hence dlsym, not a link-time reference), and the
// possibility that kernel start/end — which originate on the GPU — are NOT
// re-based through this callback. So the substitution is VERIFIED below and
// silently declined if the check fails. Getting this wrong would not degrade
// the timeline, it would place kernels ~1.8e18 ns away from the capture.
using TimestampFn = uint64_t(CUPTIAPI*)(void);
using RegisterTimestampFn = CUptiResult (*)(TimestampFn);

uint64_t CUPTIAPI tsc_now() {
    return pcomm::helpers::ts::RdtscTimestampSource::Now();
}

// Which domain kernel records are stamped in — decided at arm time by the
// check in try_tsc_timebase(), never guessed.
pcomm::core::DomainId g_domain = pcomm::core::kDomainCuptiWall;

// Returns true when CUPTI is confirmed to be reporting our TSC.
bool try_tsc_timebase() {
    if (const char* off = std::getenv("PCOMM_CUPTI_TSC"); off && *off == '0')
        return false;

    auto reg = reinterpret_cast<RegisterTimestampFn>(
        dlsym(RTLD_DEFAULT, "cuptiActivityRegisterTimestampCallback"));
    if (!reg) {
        if (g_verbose)
            std::fprintf(stderr, "[pcomm/cupti] no cuptiActivityRegisterTimestampCallback — "
                                 "staying on CUPTI's own timebase\n");
        return false;
    }
    if (CUptiResult r = reg(tsc_now); r != CUPTI_SUCCESS) {
        if (g_verbose)
            std::fprintf(stderr, "[pcomm/cupti] timestamp callback refused: %s\n", cupti_err(r));
        return false;
    }

    // The check: ask CUPTI what time it is, and ask the TSC, back to back. If
    // the substitution took, the two are the same clock and differ by the cost
    // of the call. If it did not, they differ by ~1.8e18 — a CLOCK_REALTIME ns
    // against a tick count — so the threshold does not need to be delicate.
    // A millisecond of TSC is ~2e6 ticks; anything under a second is proof.
    uint64_t before = pcomm::helpers::ts::RdtscTimestampSource::Now();
    uint64_t cupti_ts = 0;
    if (cuptiGetTimestamp(&cupti_ts) != CUPTI_SUCCESS)
        return false;
    uint64_t   after = pcomm::helpers::ts::RdtscTimestampSource::Now();
    const bool bracketed = cupti_ts >= before && cupti_ts <= after + 10'000'000ull;
    if (!bracketed && g_verbose)
        std::fprintf(stderr,
                     "[pcomm/cupti] timestamp callback registered but CUPTI still reports its own "
                     "clock (%llu vs tsc %llu) — staying on domain %u\n",
                     (unsigned long long)cupti_ts, (unsigned long long)before,
                     unsigned{pcomm::core::kDomainCuptiWall});
    return bracketed;
}

// (cupti_ticks, wall_ns) sampled as close together as we can manage. Two of
// these — one at enable, one at teardown — are what let pcomm-read fit domain 1
// onto the shared timeline. Without them every kernel row is dropped and
// counted in link_health.timeline_unanchored.
//
// A NO-OP once the TSC substitution is confirmed: records are then in domain 0,
// which the engine already anchors, and emitting a second pair here would
// describe a domain no record is in. Left in place rather than deleted because
// the fallback is a runtime decision, not a build-time one.
void emit_anchor() {
    if (g_domain == pcomm::core::kDomainHostTsc)
        return;
    uint64_t cupti_ts = 0;
    if (cuptiGetTimestamp(&cupti_ts) != CUPTI_SUCCESS)
        return;
    pcomm_engine_clock_anchor(pcomm::core::kDomainCuptiWall, cupti_ts, wall_ns());
}

void CUPTIAPI on_buffer_request(uint8_t** buf, size_t* size, size_t* max_records) {
    void* p = nullptr;
    if (posix_memalign(&p, kBufAlign, kBufSize) != 0)
        p = nullptr;
    *buf = static_cast<uint8_t*>(p);
    *size = p ? kBufSize : 0;
    *max_records = 0;  // fill by size, not by count
}

void CUPTIAPI on_buffer_complete(CUcontext, uint32_t, uint8_t* buf, size_t, size_t valid) {
    if (!buf)
        return;
    CUpti_Activity* rec = nullptr;
    // This runs on a CUPTI worker thread, so the records land in that thread's
    // arena. That is fine and deliberate: arenas are registered with the
    // ThreadBufferRegistry, so Tracer::finalize() drains threads that never
    // call drain_thread() themselves — the same path NVBit's receive thread and
    // NCCL's proxy threads already rely on.
    while (cuptiActivityGetNextRecord(buf, valid, &rec) == CUPTI_SUCCESS) {
        if (rec->kind != CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL)
            continue;
        auto*          k = reinterpret_cast<CUpti_ActivityKernel9*>(rec);
        const uint32_t grid[3] = {static_cast<uint32_t>(k->gridX), static_cast<uint32_t>(k->gridY),
                                  static_cast<uint32_t>(k->gridZ)};
        const uint16_t block[3] = {static_cast<uint16_t>(k->blockX),
                                   static_cast<uint16_t>(k->blockY),
                                   static_cast<uint16_t>(k->blockZ)};
        pcomm_engine_record_kernel(k->start, k->end, g_domain, k->name, k->correlationId,
                                   // streamId, not a CUstream: CUPTI reports the ordinal, which is
                                   // what a timeline lane wants anyway.
                                   k->streamId, static_cast<uint16_t>(k->deviceId),
                                   static_cast<uint8_t>(pcomm::core::KernelSource::Cupti), grid,
                                   block);
        g_kernels.fetch_add(1, std::memory_order_relaxed);
    }
    size_t dropped = 0;
    if (cuptiActivityGetNumDroppedRecords(nullptr, 0, &dropped) == CUPTI_SUCCESS && dropped)
        g_dropped.fetch_add(dropped, std::memory_order_relaxed);
    free(buf);
}

const char* cupti_err(CUptiResult r) {  // NOLINT — forward-declared above
    const char* m = nullptr;
    cuptiGetResultString(r, &m);
    return m ? m : "(no message)";
}

// Runs exactly once, from whichever arming door is reached first. `g_armed_by`
// records which, because that is the difference between "interposition worked"
// and "we fell back to the constructor" and it is not guessable from outside.
void enable_once() {
    g_verbose = std::getenv("PCOMM_VERBOSE") != nullptr;

    // Two questions, and this backend is the only thing in the tree that has to
    // ask both. They fail for different reasons and so they say different
    // things: "nobody wants kernel times" is the user's choice, "you may not
    // take CUPTI" is the launcher protecting a gpumem capture that is still
    // being timed by NVBit. Reporting either as the other is how the two got
    // conflated in the first place — see engine_abi.h.
    if (!pcomm_engine_kernel_timing_wanted()) {
        if (g_verbose)
            std::fprintf(stderr, "[pcomm/cupti] kernel timing off (PCOMM_KERNEL_TIMING=0)\n");
        return;
    }
    if (!pcomm_engine_cupti_allowed()) {
        if (g_verbose)
            std::fprintf(stderr, "[pcomm/cupti] not taking CUPTI — NVBit owns this process's "
                                 "subscriber\n");
        return;
    }

    // BEFORE anything is enabled, as SigmaTrace does it: the timebase has to be
    // in place before CUPTI stamps its first record, or the capture would hold
    // two clocks under one domain id.
    if (try_tsc_timebase())
        g_domain = pcomm::core::kDomainHostTsc;

    CUptiResult r = cuptiActivityRegisterCallbacks(on_buffer_request, on_buffer_complete);
    if (r != CUPTI_SUCCESS) {
        // The expected, survivable outcome when NVBit owns the process. Said
        // out loud rather than swallowed: a capture with no kernel rows and no
        // explanation is indistinguishable from a run that launched no kernels.
        std::fprintf(stderr,
                     "[pcomm/cupti] kernel timing UNAVAILABLE: %s\n"
                     "              Another CUDA tool owns this process's CUPTI subscriber "
                     "(NVBit, via the gpumem provider).\n"
                     "              Comm capture is unaffected; GpuKernel rows will be absent.\n",
                     cupti_err(r));
        return;
    }
    r = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL);
    if (r != CUPTI_SUCCESS) {
        std::fprintf(stderr, "[pcomm/cupti] cuptiActivityEnable failed: %s\n", cupti_err(r));
        return;
    }

    emit_anchor();
    g_on.store(true, std::memory_order_release);
    if (g_verbose)
        std::fprintf(stderr, "[pcomm/cupti] kernel timing on (domain %u, %s timebase)\n",
                     unsigned{g_domain},
                     g_domain == pcomm::core::kDomainHostTsc ? "host TSC" : "CUPTI wall");
}

const char* g_armed_by = nullptr;

inline void arm(const char* who) {
    std::call_once(g_once, [who] {
        g_armed_by = who;
        enable_once();
    });
}

// The launch interposers exist ONLY to provide a late, CUDA-is-definitely-up
// moment to arm from. They add one relaxed atomic load per launch after the
// first and never inspect their arguments — the timing itself comes from
// CUPTI's activity records, not from here, because a launch-time host stamp is
// submission and submission trails execution by 9-13 ms.
template<typename Fn> Fn next(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

}  // namespace

extern "C" {

// cuInit is the preferred arming point: the driver runs CUDA_INJECTION64_PATH
// tools (NVBit) from inside it, so arming AFTER the real call means NVBit has
// already taken the CUPTI subscriber and we get the soft error instead of
// killing it.
CUresult cuInit(unsigned flags) {
    static auto real = next<CUresult (*)(unsigned)>("cuInit");
    const auto  rc = real ? real(flags) : CUDA_ERROR_NOT_INITIALIZED;
    arm("cuInit");
    return rc;
}

CUresult cuLaunchKernel(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx,
                        unsigned by, unsigned bz, unsigned shmem, CUstream s, void** params,
                        void** extra) {
    static auto real =
        next<CUresult (*)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                          unsigned, CUstream, void**, void**)>("cuLaunchKernel");
    arm("cuLaunchKernel");
    return real ? real(f, gx, gy, gz, bx, by, bz, shmem, s, params, extra)
                : CUDA_ERROR_NOT_INITIALIZED;
}

CUresult cuLaunchKernelEx(const CUlaunchConfig* cfg, CUfunction f, void** params, void** extra) {
    static auto real =
        next<CUresult (*)(const CUlaunchConfig*, CUfunction, void**, void**)>("cuLaunchKernelEx");
    arm("cuLaunchKernelEx");
    return real ? real(cfg, f, params, extra) : CUDA_ERROR_NOT_INITIALIZED;
}

// The runtime-API launch, for an application that links libcudart dynamically.
// nvcc links it STATICALLY by default, in which case this symbol is inside the
// executable and no LD_PRELOAD can reach it — which is why it is one of several
// arming points and not the only one.
cudaError_t cudaLaunchKernel(const void* func, dim3 grid, dim3 block, void** args, size_t shmem,
                             cudaStream_t stream) {
    static auto real = next<cudaError_t (*)(const void*, dim3, dim3, void**, size_t, cudaStream_t)>(
        "cudaLaunchKernel");
    arm("cudaLaunchKernel");
    return real ? real(func, grid, block, args, shmem, stream) : cudaErrorInitializationError;
}

// Bring the capture up on load, exactly as the ibverbs provider does. This is
// the direct-LD_PRELOAD path and it does NOT touch CUPTI — starting a sink and
// a recorder is unrelated to the subscriber slot, so there is nothing here that
// could displace NVBit.
__attribute__((constructor)) void pcomm_cupti_load() {
    const pcomm::Config cfg = pcomm::Config::from_env();
    // No declare_provider call: this backend emits nothing of its own. GpuKernel
    // is the ENGINE's record, in the engine's already-declared 500-599 range,
    // and claiming a second range for it would make the disjointness check fail
    // on a fact that is not true.
    pcomm_engine_acquire(cfg.output_dir.c_str(), cfg.rank, cfg.verbose ? 1 : 0);

    // Last resort, and it must exist: an application whose launches we cannot
    // interpose (statically linked cudart reaching the driver through
    // cuGetProcAddress — measured to defeat symbol interposition entirely
    // during the IBGDA work) would otherwise never arm, and the capture would
    // have no kernel rows with nothing saying why. PCOMM_KERNEL_TIMING_LAZY=1
    // suppresses this for a hand-built LD_PRELOAD chain that also contains
    // NVBit, where losing the race matters more than losing the lane.
    if (const char* lazy = std::getenv("PCOMM_KERNEL_TIMING_LAZY"); !lazy || *lazy == '0')
        arm("constructor");
}

// Teardown. The final anchor must be written BEFORE the flush is reported, and
// both must happen while the Tracer still has a recorder — this is a
// destructor, so it runs before the engine's own exit path.
__attribute__((destructor)) void pcomm_cupti_fini() {
    if (!g_on.load(std::memory_order_acquire))
        return;
    emit_anchor();
    cuptiActivityFlushAll(1);
    // Drain the arenas this thread owns; the CUPTI worker's are reached by
    // Tracer::finalize() through the registry.
    pcomm_engine_drain_thread();
    if (g_verbose || g_dropped.load())
        std::fprintf(stderr, "[pcomm/cupti] kernels=%llu dropped=%llu armed_by=%s\n",
                     (unsigned long long)g_kernels.load(), (unsigned long long)g_dropped.load(),
                     g_armed_by ? g_armed_by : "(never)");
    pcomm_engine_release();
}

}  // extern "C"
