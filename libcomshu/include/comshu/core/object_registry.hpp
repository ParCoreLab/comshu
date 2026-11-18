#pragma once
// Application-level memory objects, owned by the core rather than by a provider.
//
// WHY IT IS HERE AND NOT IN gpumem. The question a capture must answer is "what
// did the program allocate", and until now it answered "only if you ran NVBit".
// Measured on one NVSHMEM workload: captured with mlx5dv alone it holds 20
// mem_object rows of 8, 40 and 327680 bytes — NVSHMEM's own control structures
// and work queues, because NVSHMEM registers what *it* hands the NIC — and the
// two 4096-byte buffers the application allocated are absent. Captured with
// gpumem they are there. NVBit's fixed cost is ~435 s of module lifting (case
// 05), so that made a buffer's name cost the heaviest provider in the tool.
//
// Everything else the join needs already exists: addresses are recorded even
// under dmabuf (core/dmabuf_registry.hpp recovers them from the
// cuMemGetHandleForAddressRange NVSHMEM must call to obtain the fd), and
// comshu-read's mem_owner already resolves an address to its containing object.
// Only one end of the join was missing.
//
// NOT A PROVIDER, deliberately. PLAN-20260806-1329 forbids a provider asking
// another provider for memory facts at runtime, and gpumem needs a buffer's
// extent at drain time to attribute an access — so a sibling "objects provider"
// would reintroduce exactly the coupling that rule exists to prevent. The
// registry has to sit BELOW the providers. `core/mem_attr.hpp`'s device_of is
// the same shape and the same reasoning.
//
// This does NOT make the core the monopolist PLAN-20260806-1329 rejected. That
// one would have owned every allocation sighting, including ones no single
// interposer can see. This owns allocator APIs that hand back an address;
// providers keep emitting what only they can see, and dedup is unchanged
// because everyone interns the base in InternDomain::Pointer and therefore
// mints the same id for the same buffer.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "comshu/core/ids.hpp"
#include "comshu/core/vocab.hpp"

namespace comshu::core {

// LIVENESS, NOT ERASURE. Entries are never removed: an access still draining
// after a free must still find its object, and the reader resolves reuse with
// the [open, close) window (PLAN-20260805-0053). But once plain device
// allocations are tracked (PLAN-20261118-1930), "never removed" without a
// liveness bit stops being harmless:
//
//   - a second free of a base would emit a second close for one open, which is
//     the unmatched-close signal entity_close_accounted exists to report — and
//     here it would be our own double count, not a missing door;
//   - cuMemUnmap names a RANGE and must close exactly the mappings live in it;
//   - an allocator that frees and reallocates the same buffer in a loop would
//     grow this table without bound, one dead duplicate per iteration.
//
// So an entry carries `live`. A reallocation at a (base, size) we already hold
// revives that entry instead of appending, which bounds the table by distinct
// extents rather than by calls. The interned id is minted from the base, so the
// revived entry already carries the id the new allocation would get.
class ObjectRegistry {
public:
    struct Object {
        std::uint64_t base{0};
        std::uint64_t size{0};
        Handle        object{kNullHandle};
        DeviceId      device{0xFFFF};
        bool          live{true};
    };

    static ObjectRegistry& instance() {
        // Leaked on purpose, like every other engine global: these are reachable
        // from interposers that can run during exit handlers, after ordinary
        // static destructors would have run.
        static auto* self = new ObjectRegistry();
        return *self;
    }

    void add(std::uint64_t base, std::uint64_t size, Handle object, DeviceId device) {
        std::lock_guard lock(m_mutex);
        auto&           at = m_by_base[base];
        for (auto& o : at)
            if (o.size == size) {
                o.object = object;
                o.device = device;
                o.live = true;
                return;
            }
        at.push_back({base, size, object, device, true});
        if (size > m_max_size)
            m_max_size = size;
    }

    // Closes the LIVE object registered at exactly this base and returns its
    // handle, or kNullHandle when there is none: never opened (allocated before
    // we attached — closing it would manufacture an unmatched row), or already
    // closed. free() is handed the pointer malloc returned, so an exact match is
    // what a close means; anything merely containing it is a different, larger
    // allocation.
    Handle close_at(std::uint64_t base) {
        std::lock_guard lock(m_mutex);
        auto            it = m_by_base.find(base);
        if (it == m_by_base.end())
            return kNullHandle;
        for (auto& o : it->second)
            if (o.live) {
                o.live = false;
                return o.object;
            }
        return kNullHandle;
    }

    // Closes every live object whose BASE lies in [lo, hi) and returns their
    // handles. cuMemUnmap's contract: one call may unmap several mappings, and a
    // mapping is the object, so each is closed — and only the ones that are
    // actually inside the range.
    std::vector<Handle> close_range(std::uint64_t lo, std::uint64_t hi) {
        std::lock_guard     lock(m_mutex);
        std::vector<Handle> out;
        for (auto it = m_by_base.lower_bound(lo); it != m_by_base.end() && it->first < hi; ++it)
            for (auto& o : it->second)
                if (o.live) {
                    o.live = false;
                    out.push_back(o.object);
                }
        return out;
    }

    // Innermost containing object, by value. Smallest wins because ranges nest —
    // an nvshmem_malloc'd buffer sits inside the heap region mapped once for all
    // of them — and by value because the lock ends with the call.
    //
    // A LIVE object beats a dead one at any size: after a free, the address can
    // belong to a new allocation and the old extent is history. The dead are a
    // fallback only, for an access attributed after its buffer was freed.
    //
    // Bounded scan: an object containing `addr` has base in (addr - max_size,
    // addr], so the walk stops there instead of visiting every lower base.
    std::optional<Object> find(std::uint64_t addr) const {
        std::lock_guard       lock(m_mutex);
        std::optional<Object> live, dead;
        const std::uint64_t   floor = addr > m_max_size ? addr - m_max_size : 0;
        for (auto it = m_by_base.upper_bound(addr); it != m_by_base.begin();) {
            --it;
            if (it->first < floor)
                break;
            for (const auto& o : it->second) {
                if (addr - o.base >= o.size)
                    continue;
                auto& best = o.live ? live : dead;
                if (!best || o.size < best->size)
                    best = o;
            }
        }
        return live ? live : dead;
    }

private:
    mutable std::mutex                           m_mutex;
    std::map<std::uint64_t, std::vector<Object>> m_by_base;
    std::uint64_t                                m_max_size{0};
};

}  // namespace comshu::core
