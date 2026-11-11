// The intern hot path: is a repeated key still one mutex and one hash lookup?
//
// WHY THIS EXISTS. `rec().intern()` is a call across the engine's C ABI into a
// table behind a per-domain mutex, and providers call it once per RECORD, not
// once per entity — every send interns its endpoint pointer, every access its
// object base. ucx_deep measured it at 86 ns against a 39 ns operation: four
// fifths of the provider's cost was re-deriving a handle it had already been
// given. It fixed that with a private cache in its own header, which left every
// other provider paying the same toll and the next provider free to pay it
// again.
//
// So the cache moved into the engine, and this file is what says it is a cache
// and not a bug. A wrong handle here does not corrupt one field — it points a
// record at the wrong string in the dictionary, and the capture stays
// well-formed while meaning something else entirely.
//
// The correctness argument rests on one property, asserted below rather than
// assumed: **a (domain, key) -> handle mapping is stable for the life of the
// process.** InternTable holds it in a function-local static inside a Recorder
// member function, so every Recorder shares one set of tables; nothing clears
// them; and atfork_child RELEASES the parent's recorder rather than destroying
// it, so even the child continues the same numbering. That is what makes the
// cache exact rather than approximate.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <random>
#include <thread>
#include <vector>

#include <unistd.h>

#include "comshu/engine_abi.h"
#include "comshu/rec/intern_table.hpp"
#include "comshu/tracer.hpp"

namespace fs = std::filesystem;
using namespace comshu;
using rec::InternDomain;

namespace {

int g_fail = 0;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok " : "XX ", what);
    if (!ok)
        g_fail = 1;
}

std::uint32_t intern_key(InternDomain d, std::uint64_t k) {
    return comshu_engine_intern_key(static_cast<std::uint8_t>(d), k);
}

// ns per call over `reps` interns of `keys`, cycled. Returns the median of five
// passes: this runs on a shared machine and the minimum flatters a cache while
// the mean is at the mercy of one scheduling hiccup.
double ns_per_intern(InternDomain d, const std::vector<std::uint64_t>& keys, int reps) {
    std::vector<double> passes;
    for (int p = 0; p < 5; ++p) {
        const auto    t0 = std::chrono::steady_clock::now();
        std::uint32_t sink = 0;
        for (int i = 0; i < reps; ++i)
            sink += intern_key(d, keys[static_cast<std::size_t>(i) % keys.size()]);
        const auto t1 = std::chrono::steady_clock::now();
        // Consume `sink` so the loop cannot be optimised away.
        asm volatile("" ::"r"(sink));
        passes.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / reps);
    }
    std::sort(passes.begin(), passes.end());
    return passes[2];
}

}  // namespace

int main() {
    const auto dir = fs::temp_directory_path() / ("comshu-intern-hot-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    Config cfg;
    cfg.output_dir = dir;
    cfg.rank = 0;
    Tracer::instance().start(cfg);

    // --- the property the cache rests on ------------------------------------
    //
    // Asserted, not assumed. If a future change ever gives each Recorder its own
    // tables, or clears them on any event, this is the line that goes red — and
    // it goes red HERE rather than as a capture whose handles resolve to the
    // wrong strings on a cluster three weeks later.
    const auto first = intern_key(InternDomain::Pointer, 0x1000);
    expect(first != 0, "a key interns to a non-null handle");
    bool stable = true;
    for (int i = 0; i < 1000; ++i)
        stable = stable && intern_key(InternDomain::Pointer, 0x1000) == first;
    expect(stable, "the same key returns the same handle, every time");

    // --- distinctness, which is what a wrong cache would break --------------
    //
    // 4096 keys is well past any plausible cache size, so this exercises
    // eviction: a direct-mapped cache that answered from a colliding slot would
    // hand two different keys the same handle, and the duplicate shows up here.
    std::vector<std::uint64_t> keys;
    std::vector<std::uint32_t> handles;
    for (int i = 0; i < 4096; ++i)
        keys.push_back(0x7f0000000000ull + static_cast<std::uint64_t>(i) * 64);
    for (auto k : keys)
        handles.push_back(intern_key(InternDomain::Pointer, k));
    auto sorted = handles;
    std::sort(sorted.begin(), sorted.end());
    expect(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
           "4096 distinct keys get 4096 distinct handles");
    expect(std::find(handles.begin(), handles.end(), 0u) == handles.end(),
           "no key interned to the null handle");

    // Second pass over the same keys in the same order: every one must return
    // the handle it returned the first time. This is the assertion a cache that
    // answered for the WRONG address fails, and the one that is worth more than
    // any timing number below.
    bool same = true;
    for (std::size_t i = 0; i < keys.size(); ++i)
        same = same && intern_key(InternDomain::Pointer, keys[i]) == handles[i];
    expect(same, "re-interning 4096 keys reproduces every handle exactly");

    // Interleaved, so a cached entry is repeatedly evicted and refilled rather
    // than swept once in address order.
    std::mt19937_64 rng(12345);
    bool            shuffled_ok = true;
    for (int i = 0; i < 20000; ++i) {
        const auto ix = rng() % keys.size();
        shuffled_ok = shuffled_ok && intern_key(InternDomain::Pointer, keys[ix]) == handles[ix];
    }
    expect(shuffled_ok, "20k interleaved lookups never return another key's handle");

    // --- domains must not bleed into one another ----------------------------
    //
    // Handles restart per domain, so key 0x1000 in Pointer and key 0x1000 in Pc
    // are different entries that happen to share a number. A cache keyed on the
    // value alone would answer one with the other's handle, and the record would
    // resolve against the wrong dictionary section — well-formed, and wrong.
    const auto ptr_h = intern_key(InternDomain::Pointer, 0xabc0);
    const auto pc_h = intern_key(InternDomain::Pc, 0xabc0);
    bool       domains_stable = true;
    for (int i = 0; i < 500; ++i) {
        domains_stable = domains_stable && intern_key(InternDomain::Pointer, 0xabc0) == ptr_h;
        domains_stable = domains_stable && intern_key(InternDomain::Pc, 0xabc0) == pc_h;
    }
    expect(domains_stable, "one key in two domains keeps two handles");

    // --- another thread must agree ------------------------------------------
    //
    // The cache is per-thread; the TABLE is not, and that is the whole reason
    // the tables have a mutex. A second thread must derive the same handle for
    // the same key — the instrumentation thread interning a pc that the flush
    // thread will write is exactly this case.
    std::uint32_t other = 0;
    std::thread([&] { other = intern_key(InternDomain::Pointer, 0x1000); }).join();
    expect(other == first, "a second thread gets the same handle for the same key");

    // --- what it costs ------------------------------------------------------
    //
    // NOT AN ASSERTION. This runs on whatever machine happens to build the tree,
    // and a threshold here would be a flaky test rather than a guarantee. The
    // number is printed so a regression is visible to anyone reading the log,
    // which is how the 86 ns was found in the first place.
    //
    // Two working sets, because they measure different things: 8 keys fits any
    // cache and shows the hit path; 4096 exceeds it and shows that the miss path
    // has not been made slower to buy the hit.
    std::vector<std::uint64_t> hot(keys.begin(), keys.begin() + 8);
    std::printf("  [--- ] %.1f ns/intern, working set 8 (hits)\n",
                ns_per_intern(InternDomain::Pointer, hot, 200000));
    std::printf("  [--- ] %.1f ns/intern, working set 4096 (misses)\n",
                ns_per_intern(InternDomain::Pointer, keys, 200000));

    Tracer::instance().finalize();
    fs::remove_all(dir);
    std::printf("=== intern_hot_path : %s ===\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
