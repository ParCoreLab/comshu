#pragma once
// The NCCL profiler plugin's event slots and the handles it gives NCCL for them,
// split out of nccl_profiler_plugin.cpp so tests/nccl_event_pool_test.cpp can
// drive it without NCCL (CR-23). The design argument is on EventPool below.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>

#include "comshu/core/ids.hpp"

namespace nccl_prof {

// One in-flight event. Everything is copied out of the descriptor at
// startEvent, because the descriptor is a stack temporary in NCCL's frame and
// is gone by the time stopEvent arrives — only the interned handles and plain
// integers here survive to be emitted.
//
// A tagged struct with a union would be tighter. It is spelled out flat because
// the cost is a few dozen bytes on an object that lives for the duration of one
// collective, and because a union here would be one more place where reading
// the wrong arm produces a plausible number instead of a compile error.
struct Event {
    // Bumped on every acquire. A handle carries the generation it was minted
    // with, so a stale one never matches a re-issued slot. See EventPool.
    //
    // ATOMIC, with `id` and `released`, because a child's startEvent resolves
    // its parent's handle on whatever thread NCCL is on (the proxy thread, for
    // proxy ops) while another thread may be re-issuing that very slot. Plain
    // fields let the child read a matching generation and then the NEW
    // occupant's id — a wrong parent, silently (CR-23).
    std::atomic<std::uint32_t> generation;
    std::atomic<bool>          released;  // on the free list: stop/release are no-ops
    std::uint32_t              next_free;  // slot index, kNoSlot at the end of the list

    std::atomic<std::uint64_t> id;
    std::uint64_t parent;
    std::uint64_t begin;
    std::uint64_t comm_hash;
    std::uint8_t  kind;  // EventKind

    // coll
    comshu::core::Handle func, algo, proto, datatype;
    std::uint64_t       seq_number;
    std::uint64_t       count;
    std::int32_t        root;
    std::uint8_t        n_channels;
    std::uint8_t        n_warps;

    // proxyOp
    std::int32_t  peer;
    std::int32_t  n_steps;
    std::int32_t  chunk_size;
    std::int32_t  pid;
    std::uint32_t channel_id;
    std::uint32_t is_send;
};

// --- the event pool -------------------------------------------------------
//
// THE HANDLE WE GIVE NCCL IS NOT A POINTER. It is (slot index, generation)
// packed into the void*, and that is the entire design.
//
// Why a pointer cannot work. A proxy op's parent is the COLLECTIVE event — NCCL
// sets `eDescr.parentObj = sub->taskEventHandle`
// (reference/nccl-git/src/plugin/profiler.cc:574) — and
// ncclProfilerStartProxyOpEvent runs on the PROXY THREAD while the collective is
// stopped from the application thread. So NCCL routinely hands back a handle for
// an event that has already ended. With `new`/`delete` that is a use-after-free.
// With a recycled pool it is worse: the slot has been reused, and the child
// reads a plausible id belonging to a DIFFERENT event.
//
// That is not hypothetical. It was measured on MN5 job 45350841, case 02:
// proxy op 42's parent came back as 42 — ITSELF — and ops 43 and 44 also named
// 42, because the collective they belonged to had been stopped and its slot
// re-issued to proxy op 42. Confidently wrong attribution, which is the one
// outcome worse than no attribution.
//
// A refcount does not fix this and NVIDIA's example plugin has the same hole
// (plugins/profiler/example/plugin.cc:531 increments `parent->refCount`, which
// cannot help a child that arrives after the count already reached zero).
//
// The generation does fix it, completely. Slots live in blocks that are never
// freed, so an index is always valid; a generation is stamped on every acquire,
// so a handle minted before a recycle carries the OLD generation and can never
// match. The check is exact in both directions:
//
//   generation matches  -> the slot still describes that event, and its id is
//                          correct EVEN IF the event has already stopped. A
//                          stopped-but-not-yet-reused parent is still that
//                          parent, and naming it is right.
//   generation differs   -> the slot was re-issued; report 0, "not observed".
//
// No refcount is needed, which removes the ABA problem rather than defending
// against it.
//
// A FREED SLOT IS NOT REUSED WHILE ANY UNUSED SLOT REMAINS, and that is what
// makes the linkage USEFUL rather than merely safe.
//
// The generation alone is safe: a recycled slot reports 0 instead of naming the
// wrong event. But on the first clean multi-node board (MN5 job 45359727,
// case 02) safe-alone turned every one of 320 proxy ops into parent 0 — correct,
// and useless. NCCL stops a collective long before the proxy ops it spawned
// arrive, and a LIFO free list hands that just-freed slot to the very next
// event, so the parent was always gone by the time a child asked.
//
// So the pool hands out never-used storage first and only falls back to freed
// slots, oldest-first, once it reaches its ceiling. Correctness never rests on
// that ordering — a child arriving after its parent is finally reissued still
// gets 0, never a wrong id — so the delay buys usefulness without being
// load-bearing.
class EventPool {
public:
    // Encoded handle: index in the high 32 bits, generation in the low 32.
    // Generations start at 1 so a valid handle is never null — NCCL tests
    // `if (handle)` before calling stopEvent.
    static void* encode(std::uint32_t index, std::uint32_t gen) {
        return reinterpret_cast<void*>((static_cast<std::uint64_t>(index) << 32) | gen);
    }

    Event* acquire(void** out_handle) {
        std::lock_guard<std::mutex> lk(mu_);
        std::uint32_t               index;
        if (fresh_next_ < count_.load(std::memory_order_relaxed) || grow()) {
            // Never-used storage first: nothing can hold a handle to it, so it
            // cannot disturb anyone's parent lookup.
            index = fresh_next_++;
        } else if (quarantined_ > 0) {
            // At the block ceiling. Reuse the oldest freed slot even though the
            // quarantine is short — the generation still makes a stale handle
            // report 0, so this degrades the linkage window and never the
            // correctness.
            index = pop_oldest();
        } else {
            return nullptr;
        }
        Event*              e = at(index);
        // Bump BEFORE the caller rewrites the payload: a reader that saw the
        // old generation re-checks it after reading `id` and sees this.
        const std::uint32_t gen = e->generation.load(std::memory_order_relaxed) + 1;
        e->generation.store(gen, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        e->released.store(false, std::memory_order_relaxed);
        *out_handle = encode(index, gen);
        return e;
    }

    // The event id behind a handle, or 0 if the slot has been re-issued — read
    // so that a re-issue racing the read cannot hand back the new occupant's
    // id: the generation is checked again after `id` is read.
    std::uint64_t id_of(void* handle) {
        const auto          raw = reinterpret_cast<std::uint64_t>(handle);
        const std::uint32_t index = static_cast<std::uint32_t>(raw >> 32);
        const std::uint32_t gen = static_cast<std::uint32_t>(raw);
        if (gen == 0 || index >= count_.load(std::memory_order_acquire))
            return 0;
        Event* e = at(index);
        if (e->generation.load(std::memory_order_acquire) != gen)
            return 0;
        const std::uint64_t id = e->id.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return e->generation.load(std::memory_order_relaxed) == gen ? id : 0;
    }

    // Resolve a handle to its event, or nullptr if the slot has been re-issued.
    Event* resolve(void* handle) {
        const auto          raw = reinterpret_cast<std::uint64_t>(handle);
        const std::uint32_t index = static_cast<std::uint32_t>(raw >> 32);
        const std::uint32_t gen = static_cast<std::uint32_t>(raw);
        if (gen == 0 || index >= count_.load(std::memory_order_acquire))
            return nullptr;
        Event* e = at(index);
        return e->generation.load(std::memory_order_acquire) == gen ? e : nullptr;
    }

    // resolve(), for stopEvent: also nullptr once the event has been released,
    // so a second stop of one handle emits nothing.
    Event* resolve_live(void* handle) {
        Event* e = resolve(handle);
        return e && !e->released.load(std::memory_order_acquire) ? e : nullptr;
    }

    // To the BACK of the queue, never the front. A LIFO free list hands the
    // just-stopped collective straight to the next proxy op that starts, which
    // is exactly the pairing that must not happen.
    //
    // Idempotent: a second release of the same handle used to append the slot
    // to the free list again, which linked it to itself (a cycle) and later
    // handed one slot to two events (CR-23). The generation is NOT bumped here:
    // a released event's handle must stay resolvable for parent lookups while
    // the slot is quarantined.
    void release(void* handle) {
        std::lock_guard<std::mutex> lk(mu_);
        Event* e = resolve(handle);
        if (!e || e->released.exchange(true, std::memory_order_acq_rel))
            return;
        const auto index = static_cast<std::uint32_t>(reinterpret_cast<std::uint64_t>(handle) >> 32);
        at(index)->next_free = kNoSlot;
        if (tail_ == kNoSlot)
            head_ = index;
        else
            at(tail_)->next_free = index;
        tail_ = index;
        ++quarantined_;
    }

private:
    static constexpr std::uint32_t kBlock = 512;
    static constexpr std::uint32_t kMaxBlocks = 4096;
    static constexpr std::uint32_t kNoSlot = 0xffffffffu;
    // Freed slots are only reissued once the pool runs out of never-used
    // storage, and then oldest-first. With 4096 blocks of 512 that is up to 2M
    // slots before any reuse at all, which is far more than any collective's
    // proxy ops can lag it by.
    static constexpr std::uint32_t kMaxSlots = kBlock * kMaxBlocks;

    std::atomic<Event*>        blocks_[kMaxBlocks]{};
    std::atomic<std::uint32_t> count_{0};       // slots published so far
    std::uint32_t              fresh_next_ = 0; // next never-used slot
    std::uint32_t              head_ = kNoSlot; // oldest freed
    std::uint32_t              tail_ = kNoSlot;
    std::uint32_t              quarantined_ = 0;
    std::mutex                 mu_;

    Event* at(std::uint32_t index) {
        return blocks_[index / kBlock].load(std::memory_order_acquire) + (index % kBlock);
    }

    std::uint32_t pop_oldest() {
        const std::uint32_t index = head_;
        head_ = at(index)->next_free;
        if (head_ == kNoSlot)
            tail_ = kNoSlot;
        --quarantined_;
        return index;
    }

    // Caller holds mu_.
    bool grow() {
        const std::uint32_t published = count_.load(std::memory_order_relaxed);
        const std::uint32_t block = published / kBlock;
        if (block >= kMaxBlocks)
            return false;
        auto* mem = new (std::nothrow) Event[kBlock]();
        if (!mem)
            return false;
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            mem[i].generation.store(0, std::memory_order_relaxed);
            mem[i].released.store(false, std::memory_order_relaxed);
        }
        blocks_[block].store(mem, std::memory_order_release);
        // Published only after the block pointer is visible, so an index below
        // count_ always resolves.
        count_.store(published + kBlock, std::memory_order_release);
        return true;
    }
};


}  // namespace nccl_prof
