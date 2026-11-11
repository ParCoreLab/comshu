/* ucx_deep's hot-path helpers and lookup structures, driven directly.
 *
 * WHY THIS FILE EXISTS, and it is the only reason: every assertion here was
 * first attempted through tests/ucx_deep_records.sh and could not be made to
 * fail there. The mutations that SURVIVED the record gate were
 *
 *   HandleCache::lookup ignoring its key      -> gate stayed green
 *   PointerTable::find ignoring its key       -> gate stayed green
 *   uct_send.status hardcoded to UCS_OK       -> gate stayed green
 *
 * and by inspection the same is true of iov_bytes dropping its stride multiply
 * and saturate_iovcnt truncating. Not because the gate is weak about the wrong
 * things: the selftest's six interfaces and four endpoints never collide in
 * tables sized for hundreds, and over the self transport nothing fails, nothing
 * strides and nothing scatters. A workload without collisions cannot test a key
 * comparison and a workload without failures cannot test a status, and a check
 * that cannot fail is not a check. So this drives all five deliberately.
 *
 * It needs no UCX and no engine, so it runs in EVERY configuration rather than
 * only in the one built with -DCOMSHU_PROVIDER_UCX=ON — which matters, because
 * "the thing you did not build is the thing that is broken" is the rule
 * tools/build_all.sh exists to enforce and the UCX configuration is not in it.
 */
#include <cstdio>
#include <cstdint>

#include "ucx_deep_tables.hpp"

namespace d = comshu::providers::ucx_deep::detail;

static int g_fail = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail = 1;
    }
}

/* Two addresses that MUST land in the same bucket: bucket_of shifts right by 4
 * and masks, so anything 16*Slots apart collides exactly. Constructed rather
 * than hoped for — the whole point is that the collision is certain. */
template<std::size_t Slots> static const void* colliding_with(const void* p, int n) {
    return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(p) +
                                         static_cast<std::uintptr_t>(n) * 16 * Slots);
}

static void test_handle_cache() {
    std::printf("HandleCache\n");
    constexpr std::size_t N = 16;
    d::HandleCache<N>     cache;

    /* Aligned like an allocator's output, which is what the shift assumes. */
    const void* a = reinterpret_cast<const void*>(std::uintptr_t{0x10000});
    const void* b = colliding_with<N>(a, 1);
    const void* c = colliding_with<N>(a, 2);

    check(d::bucket_of<N>(a) == d::bucket_of<N>(b), "the collision is actually a collision");
    check(cache.lookup(a) == 0, "an empty cache misses");

    cache.store(a, 111);
    check(cache.lookup(a) == 111, "a stored handle is returned");
    check(cache.lookup(b) == 0, "a DIFFERENT pointer in the same bucket misses");

    /* THE MUTATION THIS FILE WAS WRITTEN FOR. Dropping the key comparison in
     * lookup() makes this line return 111 for b's address — a wrong handle that
     * is perfectly well-formed, so every downstream referential-integrity check
     * still passes and the operation is silently attributed to another lane. */
    cache.store(b, 222);
    check(cache.lookup(b) == 222, "the evicting entry answers for itself");
    check(cache.lookup(a) == 0, "the EVICTED entry misses rather than answering wrongly");
    check(cache.lookup(c) == 0, "a third colliding pointer misses");

    /* Non-colliding entries coexist. */
    const void* far = reinterpret_cast<const void*>(std::uintptr_t{0x10000} + 16);
    cache.store(far, 333);
    check(cache.lookup(far) == 333, "a pointer in another bucket is unaffected");
    check(cache.lookup(b) == 222, "...and does not disturb the one already there");

    /* Handle 0 is kNullHandle and is the miss signal, so it must never be
     * cached as a hit — a caller storing it would otherwise re-intern forever,
     * which is slow but correct, and that is the trade this asserts is the one
     * being made. */
    const void* z = reinterpret_cast<const void*>(std::uintptr_t{0x20000});
    cache.store(z, 0);
    check(cache.lookup(z) == 0, "a null handle reads as a miss, never as a hit");
}

struct Entry {
    const void* key{nullptr};
    int         payload{0};
};

static void test_pointer_table() {
    std::printf("PointerTable\n");
    constexpr std::size_t            Slots = 8;
    d::PointerTable<Entry, 4, Slots> table;

    const void* a = reinterpret_cast<const void*>(std::uintptr_t{0x40000});
    const void* b = colliding_with<Slots>(a, 1);
    const void* c = colliding_with<Slots>(a, 2);
    check(d::bucket_of<Slots>(a) == d::bucket_of<Slots>(b), "a and b collide");
    check(d::bucket_of<Slots>(a) == d::bucket_of<Slots>(c), "a and c collide");

    check(table.find(a) == nullptr, "an empty table finds nothing");
    check(table.size() == 0, "an empty table is empty");

    Entry* ea = table.insert(a);
    check(ea != nullptr, "insert into an empty table succeeds");
    ea->payload = 1;
    Entry* eb = table.insert(b);
    check(eb != nullptr, "a colliding insert succeeds");
    eb->payload = 2;
    Entry* ec = table.insert(c);
    check(ec != nullptr, "a second colliding insert succeeds");
    ec->payload = 3;
    check(table.size() == 3, "three distinct keys are three entries");

    /* THE MUTATION THIS HALF WAS WRITTEN FOR. Dropping the key comparison in
     * find() makes all three of these return the entry for `a`, so a trampoline
     * would call ANOTHER transport's function pointer with this transport's
     * endpoint — a crash in the application, caused by the tracer, and one the
     * record gate cannot provoke because its interfaces never collide. */
    check(table.find(a) == ea && ea->payload == 1, "a resolves to a");
    check(table.find(b) == eb && eb->payload == 2, "b resolves to b, not a");
    check(table.find(c) == ec && ec->payload == 3, "c resolves to c, not a");

    /* Re-inserting a live key returns the SAME entry rather than a second one.
     * That is what makes pointer re-use safe: an interface closed and another
     * opened at the same address overwrites in place instead of leaking a slot
     * and instead of leaving a stale entry findable. */
    check(table.insert(a) == ea, "re-inserting a live key returns its entry");
    check(table.size() == 3, "...and consumes no storage");

    check(table.find(reinterpret_cast<const void*>(std::uintptr_t{0x99990})) == nullptr,
          "an absent key is absent");

    /* Full. The table must DEGRADE, never overrun: the provider leaves the
     * interface uninstrumented and says so, which costs capture rather than the
     * run. ContextSlots::allocate makes the same trade for the same reason. */
    const void* d4 = reinterpret_cast<const void*>(std::uintptr_t{0x50000});
    check(table.insert(d4) != nullptr, "the fourth of four fits");
    const void* d5 = reinterpret_cast<const void*>(std::uintptr_t{0x60000});
    check(table.insert(d5) == nullptr, "the fifth is REFUSED, not written past the end");
    check(table.size() == 4, "a refused insert stores nothing");
    check(table.find(d5) == nullptr, "a refused key is not findable");
    check(table.find(a) == ea, "the entries that fit are untouched by the refusal");
}

/* The three length rules. The record gate cannot reach any of them: over the
 * self transport nothing fails, nothing strides and nothing scatters, so a
 * mutation that hardcodes success, drops the stride multiply or truncates the
 * iov count survives it. Measured — those mutations were run. */
static void test_classify_bcopy() {
    std::printf("classify_bcopy\n");
    check(d::classify_bcopy(0).payload == 0 && d::classify_bcopy(0).status == 0,
          "a zero-byte pack is zero bytes and success");
    check(d::classify_bcopy(64).payload == 64 && d::classify_bcopy(64).status == 0,
          "a positive return is the packed length");
    /* UCS_ERR_NO_RESOURCE is -6 in ucs/type/status.h. The number matters less
     * than the sign: what must never happen is 18446744073709551610 appearing
     * in a bytes column, which is what an unsigned cast of -6 produces and what
     * would then dominate every sum over uct_send. */
    const auto err = d::classify_bcopy(-6);
    check(err.status == -6, "a negative return is carried as the status");
    check(err.payload == 0, "a negative return contributes ZERO bytes, not 2^64-6");
    check(d::classify_bcopy(-1).payload == 0, "...for every negative, not just that one");
}

struct FakeIov {
    std::size_t length;
    unsigned    count;
};

static void test_iov_bytes() {
    std::printf("iov_bytes\n");
    check(d::iov_bytes<FakeIov>(nullptr, 0) == 0, "an empty iov is zero bytes");
    const FakeIov one[] = {{100, 1}};
    check(d::iov_bytes(one, 1) == 100, "one contiguous element");
    const FakeIov two[] = {{100, 1}, {28, 1}};
    check(d::iov_bytes(two, 2) == 128, "two elements sum");
    /* THE MUTATION THIS EXISTS FOR: `total += iov[i].length` instead of
     * `count * length` gives 100 here, not 700. UCX's own uct_iov_get_length
     * multiplies, and the transports that stride are the mlx5 zcopy paths — so
     * the error would appear only on real hardware and only as an
     * under-count. */
    const FakeIov strided[] = {{100, 7}};
    check(d::iov_bytes(strided, 1) == 700, "a strided element is count * length");
    const FakeIov mixed[] = {{8, 3}, {16, 2}, {1, 0}};
    check(d::iov_bytes(mixed, 3) == 56, "count 0 contributes nothing");
}

static void test_saturate_iovcnt() {
    std::printf("saturate_iovcnt\n");
    check(d::saturate_iovcnt(0) == 0, "zero");
    check(d::saturate_iovcnt(255) == 255, "the largest exact value");
    /* Truncation would make 256 read as 0 — a door that carries no iov at all,
     * which is a different and perfectly plausible-looking fact. */
    check(d::saturate_iovcnt(256) == 255, "256 saturates rather than wrapping to 0");
    check(d::saturate_iovcnt(100000) == 255, "and stays saturated");
}

static void test_bucket_shift() {
    std::printf("bucket_of\n");
    /* Without the >> 4 every 16-byte-aligned pointer lands in bucket 0 and the
     * table degenerates to a linear scan. Sixteen consecutive aligned pointers
     * must therefore occupy sixteen distinct buckets. */
    constexpr std::size_t N = 16;
    bool                  seen[N] = {};
    for (std::size_t i = 0; i < N; ++i) {
        const void* p = reinterpret_cast<const void*>(std::uintptr_t{0x30000} + i * 16);
        seen[d::bucket_of<N>(p)] = true;
    }
    std::size_t distinct = 0;
    for (bool s : seen)
        distinct += s ? 1 : 0;
    check(distinct == N, "16 consecutive aligned pointers occupy 16 distinct buckets");
}

int main() {
    std::printf("ucx_deep hot-path tables\n");
    test_handle_cache();
    test_pointer_table();
    test_bucket_shift();
    test_classify_bcopy();
    test_iov_bytes();
    test_saturate_iovcnt();
    std::printf(g_fail ? "FAIL\n" : "PASS\n");
    return g_fail;
}
