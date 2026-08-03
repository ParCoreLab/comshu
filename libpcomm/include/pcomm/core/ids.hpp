#pragma once
// L0 — fundamental id types and stable record-type-id ranges.
// See ../UNIFICATION.md §3.4 (stable, range-allocated ids).

#include <cstdint>

namespace pcomm::core {

using DomainId = std::uint16_t;      // clock domain (per CPU core / per GPU / per node)
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
inline constexpr RecordTypeId cupti_lo = 300, cupti_hi = 399;
inline constexpr RecordTypeId libfabric_lo = 400, libfabric_hi = 499;
inline constexpr RecordTypeId internal_lo = 500, internal_hi = 599;
inline constexpr RecordTypeId ucx_lo = 600, ucx_hi = 699;
inline constexpr RecordTypeId gpumem_lo = 700, gpumem_hi = 799;      // Snoopie engine
inline constexpr RecordTypeId ucx_deep_lo = 800, ucx_deep_hi = 899;  // ucTrace (deferred)
}  // namespace id_range

}  // namespace pcomm::core
