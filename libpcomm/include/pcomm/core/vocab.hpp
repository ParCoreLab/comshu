#pragma once
// L0 — vendor-neutral vocabulary. No cuda*/hip*/ibv_* types may appear here;
// providers translate vendor enums into these. See ../UNIFICATION.md §6.

#include <cstdint>

namespace pcomm::core {

using GpuId = std::uint16_t;
using DeviceId = std::uint16_t;

// A device ordinal we could not determine. Not 0 — 0 is a real GPU, and the
// whole failure family this tree keeps hitting is a plausible value standing in
// for a missing one.
//
// Lives beside DeviceId rather than in a provider because every provider that
// holds an address may answer this question (see core/mem_attr.hpp); it was in
// providers/mlx5dv/mlx5_records.hpp when mlx5dv was the only one that did.
inline constexpr DeviceId kUnknownDevice = 0xFFFF;

// Where a GpuKernel's [start, end] came from. Carried as a field rather than
// split into two record types, because it is one measurement with two possible
// instruments — the "independent emitters, dedup by identity" rule.
//
// It has to be recorded, not inferred, because the instruments do not agree
// about what they measure or how well. Measured 2026-08-13 (probes in
// tests/hpc/probes/kerneltime):
//
//   Cupti             device execution bounds, on CLOCK_REALTIME already, so
//                     domain 1 needs no calibration. Unavailable whenever NVBit
//                     is loaded — it takes the process's one CUPTI subscriber.
//   NvbitGlobaltimer  device execution bounds from an injected prologue and
//                     epilogue. Its own clock domain, drifting per GPU
//                     (-13.9 ppm on H100, +5.7 ppm on A100), so it is only
//                     placeable through a per-GPU ClockAnchor.
//   HostSubmit        the launch API call, NOT execution. Kept for the case
//                     where neither of the above is available, and it is a
//                     different quantity: in the async regime every real
//                     workload uses, submission trails execution by 9-13 ms
//                     against kernels lasting 11-20 us. A consumer that draws
//                     it as a lifetime is drawing fiction, which is exactly
//                     why the source is on every row.
enum class KernelSource : std::uint8_t { Cupti = 0, NvbitGlobaltimer = 1, HostSubmit = 2 };

// How much a provider records. Ordered by volume, and compared with <= so a
// new mode slots in without touching every call site.
//
// The numbers that motivate this, from the SigmaTrace paper (SC26, Fig. 5 and
// Table I) whose vocabulary this deliberately reuses: on NCCL AllReduce at
// 32 nodes / 128 GPUs, Aggregate and Simple stay under 1.5 % overhead at every
// scale while FULL costs 8-13 %, and the trace goes from 1.7 GB (Simple) to
// 13 GB (Full). The paper attributes that gap to exactly the streams gated
// here: "recording large numbers of ibv_poll_cq completions and ibv_post_send
// and ibv_post_recv work request data".
//
// pcomm recorded Full unconditionally until this existed, which is why our own
// 64-rank hub capture is 5.86 M poll_cq rows beside 5.83 M post_send — 45 % of
// every timeline event, for a stream that mostly says "the completion landed".
//
// FULL IS THE DEFAULT, and Simple is the opt-in. The cost above is real, and
// it is still the wrong thing to optimise by default, because the two mistakes
// are not symmetric: a capture that cost 13 % can be taken again cheaply, while
// a capture missing the stream you turn out to need cannot be taken again at
// all once the job and the allocation are gone. A queue slot is the scarce
// resource here, not a percent of wall time.
//
// Simple is worth reaching for deliberately — a capture answers "who talked to
// whom, when, how much" from the requests alone, so at scale, or on a run whose
// question is known in advance, dropping completions removes 45 % of the rows
// for nothing lost. What it costs is latency work, receive attribution and the
// CQE probe, all of which need to know when the hardware finished and what it
// reported.
enum class TraceMode : std::uint8_t {
    // Structure and volume, no per-operation events. NOT IMPLEMENTED YET: it
    // needs a counter record, because entities alone would leave comm_edges
    // empty and a capture that cannot say how many bytes moved is not the
    // cheap mode, it is the useless one. Named here so the ordering is stable.
    Aggregate = 0,
    // Requests, not completions. post_send / post_recv / modify_qp and every
    // entity; no poll_cq, no recv_cqe.
    Simple = 1,
    // Everything the provider can see, including completions. What every
    // capture before this flag existed recorded.
    Full = 2,
};

// What a CudaMemcpy moved, and between which two kinds of memory. The neutral
// spelling of CUpti_ActivityMemcpyKind; providers translate.
//
// PeerToPeer and DeviceToDevice are DIFFERENT VALUES AND NEITHER IS THE EDGE
// TEST. This vocabulary describes the API that was called — which is a claim
// about intent — while whether bytes crossed a device boundary is a fact about
// `src_device` and `dst_device`, and the two can disagree. A
// cudaMemcpy(dst_on_gpu1, src_on_gpu0, DeviceToDevice) under unified
// addressing is a cross-device transfer spelled as a local one. So the derived
// layer keys its edge on `src_device <> dst_device` and never on this enum,
// and this enum is kept because "what did the program call" is still worth
// recording — it is the difference between a workload that knows it is doing
// peer traffic and one that does not.
//
// The five Array values are the CUDA-array (texture/surface) copies. Kept
// one-for-one rather than collapsed into a single `Array` bucket: collapsing
// would lose the direction, and a direction that was recorded and then thrown
// away is worse than one that was never available.
enum class MemcpyKind : std::uint8_t {
    Unknown = 0,
    HostToDevice = 1,
    DeviceToHost = 2,
    DeviceToDevice = 3,  // one device to ITSELF, as the API was called
    HostToHost = 4,
    PeerToPeer = 5,  // two devices, as the API was called
    HostToArray = 6,
    ArrayToHost = 7,
    ArrayToArray = 8,
    ArrayToDevice = 9,
    DeviceToArray = 10,
};

// Where one end of a copy's bytes live. The neutral spelling of
// CUpti_ActivityMemoryKind.
//
// NOT MemSpace, which is a different question with a confusingly similar name:
// that one is the SASS address space of an instruction (global/local/shared)
// and answers "which memory did this thread address". This one answers "what
// kind of allocation is this end of the transfer", and the distinction that
// makes it worth recording is Pageable vs Pinned — a host buffer that is not
// page-locked forces the driver to stage the copy through its own bounce
// buffer, which is a factor-of-several bandwidth fact that no other record in
// a capture can state.
enum class MemResidency : std::uint8_t {
    Unknown = 0,
    Pageable = 1,
    Pinned = 2,
    Device = 3,
    Array = 4,
    Managed = 5,
    DeviceStatic = 6,
    ManagedStatic = 7,
};

// Where a CudaMemcpy's [start, end] came from. KernelSource's argument, on a
// second measurement: it has to be recorded rather than inferred because the
// instruments do not measure the same quantity.
//
//   Cupti    the COPY ENGINE's execution window, from CUPTI's activity API.
//            The only source that exists today, and the only one that names
//            both device ordinals of a peer copy (CUpti_ActivityMemcpyPtoP4
//            carries srcDeviceId/dstDeviceId; the API arguments do not, they
//            carry pointers).
//   HostApi  the API call. For a synchronous cudaMemcpy that is a genuine
//            bound — the call blocks until the transfer completes — and for
//            cudaMemcpyAsync it is SUBMISSION, which trails execution the same
//            9-13 ms a kernel launch does. NO PRODUCER YET. It is declared now
//            because the interval's MEANING depends on it, so a consumer that
//            filters on it (see _tl_raw) is written before the row it has to
//            exclude exists rather than after — the same choice _nvlink_sends
//            made for its Load branch.
enum class CopySource : std::uint8_t { Cupti = 0, HostApi = 1 };

enum class MemOp : std::uint8_t { Load, Store, Atomic, Prefetch };

enum class MemSpace : std::uint8_t { Global, Local, Shared };

struct MemRange {
    std::uint64_t base{0};
    std::uint64_t size{0};
};

// Predicates for Edge records (extend as attribution grows).
//
// WHEN A RELATION EARNS ONE is in libpcomm/schema/README.md, under "When a
// relation earns an Edge". Short version: inline it as a field when it is 1:1,
// known at write time, and hot; use an Edge when it is 1:N, TIME-VARYING, or
// LEARNED LATER. The last is not a preference — a stream is a packed array with
// no index, so a field can never be filled in afterwards, and an Edge is the
// only shape that can carry a fact discovered after the record was written.
//
// All four below are currently served by an inline field or a reader-side join,
// so `Attribution` has no producer today. It is the one-of-each-archetype
// example records.hpp seeded so the writer and reader exercise the Edge path.
enum class EdgePredicate : std::uint8_t {
    TouchedObject,     // access -> mem object
    MapsToSourceLine,  // pc -> source line
    ResidesOn,         // object -> device
    MapsToQp,          // endpoint -> queue pair
};

}  // namespace pcomm::core
