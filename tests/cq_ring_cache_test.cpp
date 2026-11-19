// CR-04: the CQE probe's ring cache forgets a CQ when it is destroyed and every
// CQ of a context when the device is closed, so a CQ allocated at a recycled
// address resolves its OWN ring instead of reading a dead one's.
#include <cassert>
#include <cstdio>

#include "cq_ring_cache.hpp"

namespace {
struct Ring {
    int id = 0;
};
}  // namespace

int main() {
    ibv::cqeprobe::CqRingCache<Ring> c;
    int  ctx_a = 0, ctx_b = 0;
    char cq_storage[3];
    const void* cq1 = &cq_storage[0];
    const void* cq2 = &cq_storage[1];
    const void* cq3 = &cq_storage[2];

    assert(c.find(cq1) == nullptr);
    assert(c.insert(cq1, &ctx_a, Ring{1}).id == 1);
    assert(c.find(cq1) && c.find(cq1)->id == 1);

    // Destroy, then a new CQ at the SAME address: the old ring must be gone,
    // so ring_for() resolves the new one rather than returning the dead one.
    assert(c.forget_cq(cq1));
    assert(c.find(cq1) == nullptr);
    assert(!c.forget_cq(cq1));  // idempotent
    c.insert(cq1, &ctx_a, Ring{11});
    assert(c.find(cq1)->id == 11);

    // close_device drops that context's CQs and only those.
    c.insert(cq2, &ctx_a, Ring{2});
    c.insert(cq3, &ctx_b, Ring{3});
    assert(c.size() == 3);
    assert(c.forget_context(&ctx_a) == 2);
    assert(c.find(cq1) == nullptr && c.find(cq2) == nullptr);
    assert(c.find(cq3) && c.find(cq3)->id == 3);
    assert(c.forget_context(&ctx_a) == 0);
    assert(c.size() == 1);

    std::puts("cq_ring_cache: ok");
    return 0;
}
