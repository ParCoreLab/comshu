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

enum class MemOp : std::uint8_t { Load, Store, Atomic, Prefetch };

enum class MemSpace : std::uint8_t { Global, Local, Shared };

struct MemRange {
    std::uint64_t base{0};
    std::uint64_t size{0};
};

// Predicates for Edge records (extend as attribution grows).
enum class EdgePredicate : std::uint8_t {
    TouchedObject,     // access -> mem object
    MapsToSourceLine,  // pc -> source line
    ResidesOn,         // object -> device
    MapsToQp,          // endpoint -> queue pair
};

}  // namespace pcomm::core
