#pragma once
// L0 — fundamental id types and stable record-type-id ranges.
// See ../UNIFICATION.md §3.4 (stable, range-allocated ids).

#include <cstdint>

namespace comshu::core {

using DomainId = std::uint16_t;  // clock domain (per CPU core / per GPU / per node)

// Clock domains. Every domain needs its own ClockAnchor pair (start and end) or
// comshu-read cannot place its records on the shared timeline and drops them —
// link_health.timeline_unanchored is the count of that happening.
//
// 0 is the host TSC, anchored by the Tracer itself.
//
// 1 is CUPTI's timebase. It gets a domain of its own even though it was
// measured to BE CLOCK_REALTIME to within 162 ns (H100) / 260 ns (A100),
// because "these two clocks agree today on these two GPUs" is a measurement,
// not a guarantee, and the anchored fit costs nothing and stays correct if it
// ever stops being true. The fit simply comes out at 1.0 ns/tick.
//
// 2 + gpu_ordinal is a device globaltimer. These genuinely need the fit: drift
// was measured at -13.86 ppm on H100 and +5.73 ppm on A100 — opposite signs, so
// no shared constant works, and 13.9 ppm over a 7 s capture is 97 us of skew
// against kernels lasting 11 us.
inline constexpr DomainId kDomainHostTsc = 0;
inline constexpr DomainId kDomainCuptiWall = 1;
inline constexpr DomainId kDomainGpuTimerBase = 2;
using Ticks = std::uint64_t;         // raw, un-converted local clock ticks
using Handle = std::uint32_t;        // interned id; 0 = null
using RecordTypeId = std::uint16_t;  // stable, range-allocated per provider

inline constexpr Handle kNullHandle = 0;

enum class RecordKind : std::uint8_t {
    Event,     // occurrence: load/store, post_send, doorbell ring, counter sample
    Entity,    // resource with a (possibly open/absent) lifetime: mem object, QP, device
    Artifact,  // computed once, timeless: SASS, PC->line table, clock calibration
    Edge,      // attribution/relation linking the above
};

// Stable RecordTypeId ranges. Existing SigmaTrace ranges are preserved;
// new ranges follow UNIFICATION.md §3.4.
namespace id_range {
inline constexpr RecordTypeId ibverbs_lo = 100, ibverbs_hi = 199;
inline constexpr RecordTypeId mlx5dv_lo = 200, mlx5dv_hi = 299;
// RESERVED AND STILL EMPTY, and by now that is a decision rather than a
// backlog item. Two records whose only instrument is CUPTI have looked at this
// range and gone to the internal range instead: GpuKernel (530) and CudaMemcpy
// (534). Both for one reason — a range names WHO OWNS the ids in it, and
// "cupti" would make a capture's answer to "do you know when kernels ran / what
// was copied" read as "only if you ran CUPTI", which is false for GpuKernel
// (NVBit measures it too) and would become false for CudaMemcpy the moment the
// host-API door lands, since interposing cudaMemcpy* needs no subscriber at
// all. Kept rather than deleted: ranges are stable ids and shrinking the map
// buys nothing.
inline constexpr RecordTypeId cupti_lo = 300, cupti_hi = 399;
inline constexpr RecordTypeId libfabric_lo = 400, libfabric_hi = 499;
inline constexpr RecordTypeId internal_lo = 500, internal_hi = 599;
inline constexpr RecordTypeId ucx_lo = 600, ucx_hi = 699;
inline constexpr RecordTypeId gpumem_lo = 700, gpumem_hi = 799;  // Snoopie engine
// providers/ucx_deep — UCX's TRANSPORT layer. Reserved for "ucTrace" and used
// for something better than reimplementing UCT internals: uct/api/uct.h
// declares every UCT data operation as an inline over
// `ep->iface->ops.ep_am_short(...)`, and that ops table is declared in the
// INSTALLED header uct/api/tl.h. So this range describes the operations UCX
// actually issued, through a door that needs no opaque offset — and on the
// HPC-X/mlx5 stack it is the only range in a capture that sees the data plane
// at all, because UCX rings its own doorbell and neither ib_post_send (110)
// nor mlx5_device_send (214) ever fires.
inline constexpr RecordTypeId ucx_deep_lo = 800, ucx_deep_hi = 899;
// NCCL is the only provider whose door is a SUPPORTED profiling interface
// rather than an interposition: every public entry point is declared through
// NCCL_API (src/include/core.h), which emits `pncclX` as a strong alias and
// leaves `ncclX` weak. So this range describes the collective layer as the
// application asked for it, not as it lands on the wire.
inline constexpr RecordTypeId nccl_lo = 900, nccl_hi = 999;
}  // namespace id_range

}  // namespace comshu::core
