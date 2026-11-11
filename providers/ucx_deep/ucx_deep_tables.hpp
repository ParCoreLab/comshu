#pragma once
// The pure helpers and lookup structures on ucx_deep's hot path, separated from
// the provider so they can be tested directly.
//
// WHY THEY LIVE IN THEIR OWN HEADER. Both were mutation-tested through the
// record gate first, and BOTH MUTATIONS SURVIVED: removing the key comparison
// from either structure left tests/ucx_deep_records.sh green, because the
// selftest's six interfaces and four endpoints never collide in a table sized
// for hundreds. The key comparison is exactly what makes the difference under
// collision, so a workload with no collisions cannot test it — and a check that
// cannot fail is not a check.
//
// So the workload gate keeps the questions it can answer (are the rows right)
// and tests/ucx_deep_tables.cpp asks the ones it cannot (what happens when two
// pointers land in one bucket, and what happens when the table fills). No UCX
// header and no comshu header is included here, so that test builds in every
// configuration rather than only in the one with UCX.

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#include <atomic>

namespace comshu::providers::ucx_deep::detail {

// Pointers out of an allocator are at least 16-byte aligned, so the low four
// bits carry no information and hashing without the shift collapses every entry
// into one bucket.
template<std::size_t Slots> constexpr std::size_t bucket_of(const void* p) noexcept {
    static_assert((Slots & (Slots - 1)) == 0, "Slots must be a power of two");
    return (reinterpret_cast<std::uintptr_t>(p) >> 4) & (Slots - 1);
}

// A direct-mapped, single-owner cache from pointer to interned handle.
//
// It exists because interning is not a hash lookup: it is a call across the
// engine's C ABI into a table behind a per-domain mutex, measured at 86 ns
// against a 39 ns operation.
//
// THE ENGINE CACHES TOO, since 2026-08-27 — that measurement was the reason,
// and libcomshu/src/engine.cpp now carries the same idea for every provider
// (83.6 -> 15.3 ns on a repeated key, tests/intern_hot_path.cpp). This one
// STAYS anyway, and the remaining 15 ns is what it buys: the engine's cache is
// reached through a cross-.so call, past Tracer::instance()'s static guard and
// a recorder check, while this one is an array probe in the calling object.
// On a 39 ns operation that difference is not a rounding error. Caching is exact rather than approximate — the
// engine's key table returns the same handle for an address forever — so the
// only way this can be wrong is by answering for the WRONG address, which is
// what the key comparison prevents and what the test drives.
//
// A miss returns 0, which is kNullHandle: never a plausible-looking wrong
// answer, always a fall-through to the real call.
template<std::size_t Slots> class HandleCache {
public:
    std::uint32_t lookup(const void* key) const noexcept {
        const Entry& e = m_entries[bucket_of<Slots>(key)];
        return e.key == key ? e.handle : 0;
    }

    void store(const void* key, std::uint32_t handle) noexcept {
        m_entries[bucket_of<Slots>(key)] = {key, handle};
    }

private:
    struct Entry {
        const void*   key{nullptr};
        std::uint32_t handle{0};
    };
    Entry m_entries[Slots]{};
};

// An open-addressed table from pointer to a fixed-lifetime entry.
//
// ENTRIES ARE NEVER REMOVED, and that is a correctness requirement rather than
// an omission: the provider's trampolines read an entry concurrently with the
// interface being closed, so erasing one would be a use-after-free inside the
// application. A key re-used after a free finds its existing entry and
// overwrites it, so pointer re-use is handled by overwrite. `insert` therefore
// never fails except by the table being full, and `find` never returns a
// dangling entry.
//
// Reads are lock-free and correct against a concurrent insert because a slot
// only ever goes null -> non-null, and the entry it names has its key written
// before the slot is published. WRITES ARE NOT: the caller serialises insert().
template<typename Entry, std::size_t MaxEntries, std::size_t Slots> class PointerTable {
    static_assert(Slots >= 2 * MaxEntries,
                  "keep the table under half full or the linear probe degenerates");

public:
    Entry* find(const void* key) const noexcept {
        std::size_t i = bucket_of<Slots>(key);
        for (std::size_t n = 0; n < Slots; ++n, i = (i + 1) & (Slots - 1)) {
            Entry* e = m_slots[i].load(std::memory_order_acquire);
            // An empty slot ends the probe: a key that existed would have been
            // placed at or before it.
            if (!e)
                return nullptr;
            if (e->key == key)
                return e;
        }
        return nullptr;
    }

    // Returns the existing entry for `key`, a fresh one, or nullptr when the
    // table is full. A fresh entry has its key set and nothing else.
    Entry* insert(const void* key) noexcept {
        std::size_t i = bucket_of<Slots>(key);
        for (std::size_t n = 0; n < Slots; ++n, i = (i + 1) & (Slots - 1)) {
            Entry* e = m_slots[i].load(std::memory_order_relaxed);
            if (e && e->key == key)
                return e;
            if (!e) {
                if (m_used >= MaxEntries)
                    return nullptr;
                Entry* fresh = &m_storage[m_used++];
                fresh->key = key;
                m_slots[i].store(fresh, std::memory_order_release);
                return fresh;
            }
        }
        return nullptr;
    }

    std::size_t size() const noexcept { return m_used; }

private:
    Entry               m_storage[MaxEntries]{};
    std::atomic<Entry*> m_slots[Slots]{};
    std::size_t         m_used{0};
};

// --- the two length rules, which are where a wrong number gets minted --------

// A *_bcopy door returns the number of bytes the pack callback produced, or a
// NEGATIVE ucs_status_t. The sign is the whole discriminator, and getting it
// wrong does not produce a small error: `static_cast<uint64_t>(-8)` is
// 18446744073709551608, which lands in a bytes column and dominates every sum
// over the table.
struct BcopyResult {
    std::uint64_t payload;
    std::int32_t  status;
};

constexpr BcopyResult classify_bcopy(::ssize_t ret) noexcept {
    if (ret < 0)
        return {0, static_cast<std::int32_t>(ret)};
    return {static_cast<std::uint64_t>(ret), 0 /* UCS_OK */};
}

// uct_iov_get_length(), spelled out: src/uct/base/uct_iov.inl:28 is
// `iov->count * iov->length`, NOT `iov->length`. A strided iov describes `count`
// elements of `length` bytes each, so dropping the multiply under-reports every
// strided zcopy by a factor of count — silently, and only on the transports that
// use striding.
//
// Templated on the iov type so this file needs no UCX header and the test can
// drive the arithmetic with a stand-in carrying the same two fields.
template<typename Iov> constexpr std::uint64_t iov_bytes(const Iov* iov, std::size_t n) noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < n; ++i)
        total += static_cast<std::uint64_t>(iov[i].count) * iov[i].length;
    return total;
}

// The record's iovcnt is one byte. Saturating rather than truncating: a
// wrap-around turns 256 scattered buffers into "0", which reads as a door that
// carries no iov at all.
constexpr std::uint8_t saturate_iovcnt(std::size_t n) noexcept {
    return static_cast<std::uint8_t>(n > 255 ? 255 : n);
}

}  // namespace comshu::providers::ucx_deep::detail
