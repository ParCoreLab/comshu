#pragma once
// The engine's own MemObject emitters. PRIVATE to libcomshu_engine: no provider
// sees this header, because providers emit their own MemObjects (they hold
// facts the engine cannot see) and reach the registry through
// comshu_engine_object_at. This is only so the engine's several allocation
// doors share one emitter.

#include <cstdint>

#include "comshu/core/vocab.hpp"

namespace comshu::engine {

// Record an application allocation [base, base + size) and register it live.
// The device is asked of the driver for the address — unless the caller already
// knows it: a VMM mapping states its device in the allocation handle, and its
// address may not be queryable yet (access is granted by cuMemSetAccess, AFTER
// the map). `known_device` < 0 means "ask".
void note_object(std::uint64_t base, std::uint64_t size, core::ObjectOrigin origin,
                 int known_device = -1);

// Close the live object at exactly `base`, if there is one. Call BEFORE the
// real free: the address can belong to a new allocation once it returns.
void close_object(std::uint64_t base);

// Close every live object whose base is in [lo, hi) — cuMemUnmap.
void close_objects_in(std::uint64_t lo, std::uint64_t hi);

// Is `pc` inside comshu's own code — the engine, or a provider that declared
// itself (and so anything statically linked into it: cudart, NVBit)? An
// allocation made from there is comshu's memory, not the application's.
bool is_comshu_code(const void* pc);

}  // namespace comshu::engine
