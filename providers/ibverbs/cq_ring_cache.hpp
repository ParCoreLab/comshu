#pragma once
// The CQE probe's per-CQ ring cache, with the lifetime it was missing (CR-04).
//
// mlx5_cqe.cpp resolves each CQ's ring (buffer, entry count, doorbell record)
// once and reads CQEs out of it on every poll. The cache used to be keyed by the
// ibv_cq* and never evicted, so a CQ allocated at a freed CQ's address inherited
// the dead CQ's ring: cqe_at() and on_poll() then read the old buffer (unmapped)
// and the old doorbell record (freed heap). The key is a heap address, so reuse
// is the allocator's normal behaviour, not an edge case.
//
// So an entry now ends with its CQ — forget_cq() before ibv_destroy_cq runs —
// and with its context, forget_context() on ibv_close_device, for any CQ the
// application did not destroy first. It remembers the context for that reason.
//
// Header-only and free of ibverbs and mlx5dv, so tests/cq_ring_cache_test.cpp
// drives it without a NIC. Not synchronised: mlx5_cqe.cpp holds its mu() around
// every call, and the references find()/insert() return are valid only under it.

#include <cstddef>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace ibv::cqeprobe {

template<typename Ring> class CqRingCache {
public:
    /** The ring cached for `cq`, or null if none. */
    const Ring* find(const void* cq) const {
        const auto it = m_.find(cq);
        return it == m_.end() ? nullptr : &it->second.ring;
    }

    /** Cache `ring` for `cq` (replacing any entry) and return the cached copy. */
    const Ring& insert(const void* cq, const void* ctx, Ring ring) {
        auto& e = m_[cq];
        e = Entry{std::move(ring), ctx};
        return e.ring;
    }

    /** Drop `cq`'s entry. True if there was one. */
    bool forget_cq(const void* cq) { return m_.erase(cq) > 0; }

    /** Drop every entry whose CQ belongs to `ctx`; returns how many. */
    std::size_t forget_context(const void* ctx) {
        std::size_t n = 0;
        for (auto it = m_.begin(); it != m_.end();) {
            if (it->second.ctx == ctx) {
                it = m_.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        return n;
    }

    std::size_t size() const { return m_.size(); }

private:
    struct Entry {
        Ring        ring{};
        const void* ctx = nullptr;
    };
    std::unordered_map<const void*, Entry> m_;
};

}  // namespace ibv::cqeprobe
