// CR-23: the NCCL profiler's event pool — a double release is a no-op (it used
// to link the slot into the free list twice), a stopped event stays resolvable
// for its children while quarantined, and a handle from before a re-issue
// names nothing rather than the new occupant.
#include <cassert>
#include <cstdio>
#include <atomic>
#include <thread>
#include <set>
#include <vector>

#include "nccl_event_pool.hpp"

int main() {
    nccl_prof::EventPool pool;
    void*                h1 = nullptr;
    nccl_prof::Event*    e1 = pool.acquire(&h1);
    assert(e1 && h1);
    e1->id.store(1001);

    pool.release(h1);
    assert(pool.id_of(h1) == 1001 && "a stopped parent stays nameable while quarantined");
    assert(pool.claim(h1) == nullptr && "a second stop of the same handle emits nothing");
    pool.release(h1);  // the double release: must not enter the free list again

    // Concurrent stops of ONE handle: exactly one claims it (the audit of
    // CR-23 — resolve-then-check outside the lock let both emit).
    for (int round = 0; round < 2000; ++round) {
        void* h = nullptr;
        assert(pool.acquire(&h));
        std::atomic<int>  winners{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                if (pool.claim(h)) {
                    winners.fetch_add(1);
                    pool.retire(h);
                }
            });
        go.store(true, std::memory_order_release);
        for (auto& t : ts)
            t.join();
        assert(winners.load() == 1 && "a concurrent double stop emitted twice");
    }

    // Drain fresh storage so the pool must reuse freed slots, then check that
    // no slot is handed to two live events at once.
    std::vector<void*> live;
    std::set<nccl_prof::Event*> seen;
    for (;;) {
        void* h = nullptr;
        nccl_prof::Event* e = pool.acquire(&h);
        if (!e)
            break;
        assert(seen.insert(e).second && "one slot handed to two live events");
        live.push_back(h);
        if (live.size() > (1u << 22))
            break;  // guard: the pool's ceiling is 2M slots
    }
    // h1's slot was re-issued exactly once; its old handle names nothing.
    assert(pool.id_of(h1) == 0 && "a re-issued slot must not report its new occupant's id");
    std::printf("nccl_event_pool: ok (%zu live slots, all distinct)\n", live.size());
    return 0;
}
