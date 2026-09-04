// libcupti_trace.so — kernel execution windows and host-initiated memory
// copies, from CUPTI's activity API.
//
// The copy half (CUDA_MEMCPY / MEMCPY2 -> CudaMemcpy, 534) was added because it
// is traffic NOTHING ELSE IN THIS TREE CAN SEE. A copy engine retires no
// instruction, so NVBit is structurally blind to it (pcomm/run.cpp says so in
// the gpumem row); it posts no work request, so ibverbs and mlx5dv are blind to
// it; it calls into neither UCX nor NCCL. And cudaMemcpyPeer between two GPUs
// is communication — it is bytes over NVLink or PCIe — so a workload built on
// it produced an empty comm_edges. This file is the only door that is inside
// the driver, which is also why it is the one that survives a statically linked
// cudart. See plans/PLAN-20260903-1828.
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
std::atomic<uint64_t> g_copies{0};
std::atomic<uint64_t> g_peer_copies{0};
std::atomic<uint64_t> g_dropped{0};
std::once_flag        g_once;
bool                  g_verbose = false;
// Whether the two memcpy activity kinds were enabled. Recorded rather than
// inferred from g_on: the copy kinds are enabled AFTER the kernel kind and
// either can fail on its own, so "kernel timing is on" does not imply "copies
// are captured" and the teardown line must be able to say which.
bool g_copies_on = false;

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

// ── memcpy activity, translated ─────────────────────────────────────────────
//
// CUPTI's vocabulary -> ours. Written as switches rather than as a table
// indexed by the CUPTI value, because the two numberings agreeing today is a
// coincidence and a table would silently mistranslate the day one of them
// gains a value in the middle.
uint8_t copy_kind(uint8_t cupti_kind) {
    using K = pcomm::core::MemcpyKind;
    switch (cupti_kind) {
        case CUPTI_ACTIVITY_MEMCPY_KIND_HTOD:
            return static_cast<uint8_t>(K::HostToDevice);
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOH:
            return static_cast<uint8_t>(K::DeviceToHost);
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOD:
            return static_cast<uint8_t>(K::DeviceToDevice);
        case CUPTI_ACTIVITY_MEMCPY_KIND_HTOH:
            return static_cast<uint8_t>(K::HostToHost);
        case CUPTI_ACTIVITY_MEMCPY_KIND_PTOP:
            return static_cast<uint8_t>(K::PeerToPeer);
        case CUPTI_ACTIVITY_MEMCPY_KIND_HTOA:
            return static_cast<uint8_t>(K::HostToArray);
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOH:
            return static_cast<uint8_t>(K::ArrayToHost);
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOA:
            return static_cast<uint8_t>(K::ArrayToArray);
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOD:
            return static_cast<uint8_t>(K::ArrayToDevice);
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOA:
            return static_cast<uint8_t>(K::DeviceToArray);
        default:
            return static_cast<uint8_t>(K::Unknown);
    }
}

uint8_t residency(uint8_t cupti_mem_kind) {
    using R = pcomm::core::MemResidency;
    switch (cupti_mem_kind) {
        case CUPTI_ACTIVITY_MEMORY_KIND_PAGEABLE:
            return static_cast<uint8_t>(R::Pageable);
        case CUPTI_ACTIVITY_MEMORY_KIND_PINNED:
            return static_cast<uint8_t>(R::Pinned);
        case CUPTI_ACTIVITY_MEMORY_KIND_DEVICE:
            return static_cast<uint8_t>(R::Device);
        case CUPTI_ACTIVITY_MEMORY_KIND_ARRAY:
            return static_cast<uint8_t>(R::Array);
        case CUPTI_ACTIVITY_MEMORY_KIND_MANAGED:
            return static_cast<uint8_t>(R::Managed);
        case CUPTI_ACTIVITY_MEMORY_KIND_DEVICE_STATIC:
            return static_cast<uint8_t>(R::DeviceStatic);
        case CUPTI_ACTIVITY_MEMORY_KIND_MANAGED_STATIC:
            return static_cast<uint8_t>(R::ManagedStatic);
        default:
            return static_cast<uint8_t>(R::Unknown);
    }
}

// Which END of a copy is on a device, from the copyKind alone.
//
// This is what turns CUPTI's ONE deviceId on a non-peer record into the record's
// two device fields without inventing anything. A host end gets
// kUnknownDevice — not 0, which is a real GPU — and a device end gets the
// context's ordinal. Both device ends of a DTOD therefore carry the SAME
// ordinal, which is precisely what makes `src_device <> dst_device` mean "CUPTI
// reported two devices" downstream rather than "we guessed".
//
// An ARRAY end counts as a device end: a CUDA array lives in device memory, and
// `src_space`/`dst_space` is where the Array-ness is recorded.
bool src_is_device(uint8_t cupti_kind) {
    switch (cupti_kind) {
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOH:
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOD:
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOA:
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOH:
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOA:
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOD:
        case CUPTI_ACTIVITY_MEMCPY_KIND_PTOP:
            return true;
        default:
            return false;
    }
}

bool dst_is_device(uint8_t cupti_kind) {
    switch (cupti_kind) {
        case CUPTI_ACTIVITY_MEMCPY_KIND_HTOD:
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOD:
        case CUPTI_ACTIVITY_MEMCPY_KIND_HTOA:
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOA:
        case CUPTI_ACTIVITY_MEMCPY_KIND_ATOD:
        case CUPTI_ACTIVITY_MEMCPY_KIND_DTOA:
        case CUPTI_ACTIVITY_MEMCPY_KIND_PTOP:
            return true;
        default:
            return false;
    }
}

// CUpti_ActivityMemcpy5 / PtoP3, NOT the newest versions the installed header
// declares (6 / 4 on CUDA 13.0). Deliberate: only the FRONT of each struct is
// read — through correlationId — and that prefix is byte-identical in every
// version from 3 upward, while the older names are declared by
// cupti_activity_deprecated.h in new toolkits and by cupti_activity.h in old
// ones. So one spelling compiles against both, and the fields it touches cannot
// move. (The existing kernel path names CUpti_ActivityKernel9 on a header whose
// newest is 10, for the same reason.)
void record_memcpy(const CUpti_ActivityMemcpy5* m) {
    const uint16_t dev = static_cast<uint16_t>(m->deviceId);
    const uint16_t none = pcomm::core::kUnknownDevice;
    pcomm_engine_record_memcpy(m->start, m->end, g_domain, m->bytes, m->correlationId, m->streamId,
                               src_is_device(m->copyKind) ? dev : none,
                               dst_is_device(m->copyKind) ? dev : none,
                               // On a non-peer record the context device IS the
                               // one device, so ctx_device repeats it. Repeating
                               // is right here and would be WRONG below, where
                               // CUPTI reports all three separately.
                               dev, copy_kind(m->copyKind),
                               residency(m->srcKind), residency(m->dstKind),
                               static_cast<uint8_t>(pcomm::core::CopySource::Cupti),
                               (m->flags & CUPTI_ACTIVITY_FLAG_MEMCPY_ASYNC) ? 1 : 0);
    g_copies.fetch_add(1, std::memory_order_relaxed);
}

// The peer record, and the only place in a capture where two device ordinals
// arrive together on one row. This is the record that makes cudaMemcpyPeer a
// comm_edges edge at all.
void record_memcpy_peer(const CUpti_ActivityMemcpyPtoP3* m) {
    pcomm_engine_record_memcpy(m->start, m->end, g_domain, m->bytes, m->correlationId, m->streamId,
                               static_cast<uint16_t>(m->srcDeviceId),
                               static_cast<uint16_t>(m->dstDeviceId),
                               // `deviceId`, NOT srcDeviceId: CUPTI reports all
                               // three on this record, and this one pairs with
                               // `contextId` — the context the copy was issued
                               // in, which belongs to the process that made it
                               // current. That is what makes it ownership
                               // evidence when the two endpoints are not.
                               static_cast<uint16_t>(m->deviceId), copy_kind(m->copyKind),
                               residency(m->srcKind), residency(m->dstKind),
                               static_cast<uint8_t>(pcomm::core::CopySource::Cupti),
                               (m->flags & CUPTI_ACTIVITY_FLAG_MEMCPY_ASYNC) ? 1 : 0);
    g_copies.fetch_add(1, std::memory_order_relaxed);
    if (m->srcDeviceId != m->dstDeviceId)
        g_peer_copies.fetch_add(1, std::memory_order_relaxed);
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
        // MEMCPY and MEMCPY2 are separate kinds with separate layouts, and the
        // split is CUPTI's rather than ours: the peer record is the only one
        // that carries TWO device ordinals, which is the whole reason a copy can
        // become an edge. Handled before the kernel branch so the `continue`
        // below stays the single "not a record we asked for" exit.
        if (rec->kind == CUPTI_ACTIVITY_KIND_MEMCPY) {
            record_memcpy(reinterpret_cast<CUpti_ActivityMemcpy5*>(rec));
            continue;
        }
        if (rec->kind == CUPTI_ACTIVITY_KIND_MEMCPY2) {
            record_memcpy_peer(reinterpret_cast<CUpti_ActivityMemcpyPtoP3*>(rec));
            continue;
        }
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

    // ── host-initiated copies ───────────────────────────────────────────────
    //
    // AFTER the kernel kind and after the subscriber is already ours, which is
    // what keeps this out of the NVBit conflict entirely: a process where NVBit
    // owns the subscriber returned above and never reaches here. Nothing in
    // pcomm/run.cpp's build_passes() changes.
    //
    // Two kinds, not one. CUPTI_ACTIVITY_KIND_MEMCPY covers H2D/D2H/D2D/H2H and
    // reports ONE device; CUPTI_ACTIVITY_KIND_MEMCPY2 is peer-to-peer and is the
    // only record in the API that names both ends. Enabling only the first would
    // capture staging traffic and miss every byte that is actually
    // communication — which is the failure this whole backend change exists to
    // fix, so it is worth stating that it is two calls and both must succeed.
    //
    // ENABLED BY DEFAULT, and PCOMM_COPY_CAPTURE=0 is the opt-out. Same
    // asymmetry core/vocab.hpp argues for TraceMode: the copy stream is small
    // (one record per transfer, against ~1050 kernels/s/rank already accepted at
    // +0.65 %), overhead can be paid again, and a capture that silently omitted
    // the only lane its workload used cannot be taken again once the allocation
    // is gone.
    //
    // Read from the environment HERE and not through engine_abi.h on purpose.
    // pcomm_engine_kernel_timing_wanted() is in the ABI because TWO backends
    // consult it and had to agree; this question has exactly one possible
    // consumer, and putting it in the ABI would imply a coordination that does
    // not exist. If a second copy instrument ever lands (the host-API door), it
    // moves — that is the same rule, applied when it becomes true.
    const char* copy_off = std::getenv("PCOMM_COPY_CAPTURE");
    if (copy_off && (*copy_off == '0' || *copy_off == 'n' || *copy_off == 'N')) {
        if (g_verbose)
            std::fprintf(stderr, "[pcomm/cupti] copy capture off (PCOMM_COPY_CAPTURE=0)\n");
    } else {
        const CUptiResult r1 = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY);
        const CUptiResult r2 = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY2);
        g_copies_on = (r1 == CUPTI_SUCCESS && r2 == CUPTI_SUCCESS);
        if (!g_copies_on)
            // Loud, not swallowed, and for the reason the kernel path above is:
            // a capture with no cuda_memcpy rows and no explanation is
            // indistinguishable from a workload that copied nothing, and the
            // peer half is indistinguishable from a workload with no peer
            // traffic. Naming which of the two calls failed matters because they
            // fail for different reasons — MEMCPY2 is the one an old CUPTI or a
            // restricted profiling mode is likelier to refuse.
            std::fprintf(stderr,
                         "[pcomm/cupti] copy capture UNAVAILABLE: memcpy=%s memcpy2=%s\n"
                         "              cuda_memcpy rows will be absent or "
                         "peer-blind; kernel timing is unaffected.\n",
                         cupti_err(r1), cupti_err(r2));
    }

    emit_anchor();
    g_on.store(true, std::memory_order_release);
    if (g_verbose)
        std::fprintf(stderr, "[pcomm/cupti] kernel timing on (domain %u, %s timebase), copies %s\n",
                     unsigned{g_domain},
                     g_domain == pcomm::core::kDomainHostTsc ? "host TSC" : "CUPTI wall",
                     g_copies_on ? "on" : "off");
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
        // `peer` is broken out of `copies` rather than left to be counted in
        // SQL: it is the number that says whether this run had any GPU-to-GPU
        // traffic at all, and a zero there beside a non-zero `copies` is the
        // difference between "the workload only staged through the host" and
        // "the peer activity kind was refused".
        std::fprintf(stderr,
                     "[pcomm/cupti] kernels=%llu copies=%llu peer=%llu dropped=%llu "
                     "armed_by=%s copies_on=%d\n",
                     (unsigned long long)g_kernels.load(), (unsigned long long)g_copies.load(),
                     (unsigned long long)g_peer_copies.load(), (unsigned long long)g_dropped.load(),
                     g_armed_by ? g_armed_by : "(never)", g_copies_on ? 1 : 0);
    pcomm_engine_release();
}

}  // extern "C"
