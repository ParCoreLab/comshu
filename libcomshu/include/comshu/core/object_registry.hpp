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
#include <mutex>
#include <optional>
#include <vector>

#include "comshu/core/ids.hpp"
#include "comshu/core/vocab.hpp"

namespace comshu::core {

class ObjectRegistry {
public:
    struct Object {
        std::uint64_t base{0};
        std::uint64_t size{0};
        Handle        object{kNullHandle};
        DeviceId      device{0xFFFF};
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
        m_objects.push_back({base, size, object, device});
    }

    // The object registered at EXACTLY this base. free() is handed the pointer
    // malloc returned, so an exact match is what a close means; anything merely
    // containing it is a different, larger allocation.
    //
    // Does not erase. Lifetime is expressed by appending EntityClose, never by
    // deleting state — a reader resolves an address reused across a free with
    // the [open, close) window, and an access still draining after the free must
    // still find its object. See PLAN-20260805-0053-entity-lifetimes.
    Handle object_at(std::uint64_t base) const {
        std::lock_guard lock(m_mutex);
        for (const auto& o : m_objects)
            if (o.base == base)
                return o.object;
        return kNullHandle;
    }

    // Innermost containing object, by value. Smallest wins because ranges nest —
    // an nvshmem_malloc'd buffer sits inside the heap region mapped once for all
    // of them — and by value because the lock ends with the call.
    std::optional<Object> find(std::uint64_t addr) const {
        std::lock_guard       lock(m_mutex);
        std::optional<Object> best;
        for (const auto& o : m_objects)
            if (o.base <= addr && addr - o.base < o.size)
                if (!best || o.size < best->size)
                    best = o;
        return best;
    }

private:
    mutable std::mutex  m_mutex;
    std::vector<Object> m_objects;
};

}  // namespace comshu::core
