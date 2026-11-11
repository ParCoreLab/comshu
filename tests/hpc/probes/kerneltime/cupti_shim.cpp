// PROBE part 5 — a preloadable prototype of "kernel timing lives in the engine".
//
// This is deliberately shaped like the real thing rather than like a benchmark:
// a constructor turns CUPTI's concurrent-kernel activity on, records land in a
// buffer, and a destructor reports. That is exactly the shape the engine would
// take (comshu_engine_acquire turning it on, finalize draining it), so what this
// measures on a real workload is what the feature would cost.
//
// It answers the two questions the microbenchmarks could not:
//   - what does CUPTI cost on a REAL collective at multi-GPU scale, on a
//     dedicated node rather than a shared login node?
//   - what is the RECORD VOLUME? A timeline stream has to be sized, and
//     "kernels per second" is the number that sizes it.
//
// It also demonstrates the graceful-degradation path that makes the whole
// design work: if NVBit already owns the process's CUPTI subscriber slot, the
// register call returns CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED, which
// is a clean, detectable error rather than a crash. The engine can try CUPTI
// and, on exactly that error, defer to gpumem's device-side instrumentation
// without the launcher having to tell it anything.
//
// Build: g++ -O2 -fPIC -shared -o cupti_shim.so cupti_shim.cpp \
//          -I$CUDA_HOME/extras/CUPTI/include -L$CUDA_HOME/extras/CUPTI/lib64 -lcupti
// Use:   LD_PRELOAD=./cupti_shim.so <app>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>

#include <cupti.h>

namespace {

std::mutex            g_mu;
std::vector<uint64_t> g_dur;                   // kernel durations, ns
uint64_t              g_lo = ~0ull, g_hi = 0;  // span of kernel activity, ns
std::atomic<uint64_t> g_count{0};
std::atomic<uint64_t> g_dropped{0};
std::atomic<uint64_t> g_bytes{0};  // activity bytes CUPTI handed back
uint64_t              g_t_start = 0;
bool                  g_on = false;

uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec;
}

constexpr size_t kBuf = 16u << 20;

void CUPTIAPI on_req(uint8_t** buf, size_t* size, size_t* max_records) {
    void* p = nullptr;
    if (posix_memalign(&p, 8, kBuf) != 0)
        p = nullptr;
    *buf = static_cast<uint8_t*>(p);
    *size = kBuf;
    *max_records = 0;
}

void CUPTIAPI on_done(CUcontext, uint32_t, uint8_t* buf, size_t, size_t valid) {
    if (!buf)
        return;
    g_bytes += valid;
    CUpti_Activity*       rec = nullptr;
    std::vector<uint64_t> local;
    uint64_t              lo = ~0ull, hi = 0;
    while (cuptiActivityGetNextRecord(buf, valid, &rec) == CUPTI_SUCCESS) {
        if (rec->kind != CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL)
            continue;
        auto* k = reinterpret_cast<CUpti_ActivityKernel9*>(rec);
        local.push_back(k->end - k->start);
        lo = std::min(lo, k->start);
        hi = std::max(hi, k->end);
    }
    size_t dropped = 0;
    cuptiActivityGetNumDroppedRecords(nullptr, 0, &dropped);
    g_dropped += dropped;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_dur.insert(g_dur.end(), local.begin(), local.end());
        g_lo = std::min(g_lo, lo);
        g_hi = std::max(g_hi, hi);
    }
    g_count += local.size();
    free(buf);
}

__attribute__((constructor)) void shim_init() {
    if (std::getenv("COMSHU_SHIM_OFF"))
        return;
    g_t_start = now_ns();
    CUptiResult r = cuptiActivityRegisterCallbacks(on_req, on_done);
    if (r != CUPTI_SUCCESS) {
        const char* m = nullptr;
        cuptiGetResultString(r, &m);
        // The load-bearing line: this is what the engine would see under NVBit.
        std::fprintf(stderr, "SHIM register failed: %s\n", m ? m : "?");
        return;
    }
    r = cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL);
    if (r != CUPTI_SUCCESS) {
        const char* m = nullptr;
        cuptiGetResultString(r, &m);
        std::fprintf(stderr, "SHIM enable failed: %s\n", m ? m : "?");
        return;
    }
    g_on = true;
    std::fprintf(stderr, "SHIM on\n");
}

__attribute__((destructor)) void shim_fini() {
    if (!g_on)
        return;
    cuptiActivityFlushAll(1);
    const double                wall_s = double(now_ns() - g_t_start) / 1e9;
    std::lock_guard<std::mutex> lk(g_mu);
    std::sort(g_dur.begin(), g_dur.end());
    const size_t n = g_dur.size();
    const double span_s = (g_hi > g_lo) ? double(g_hi - g_lo) / 1e9 : 0.0;
    uint64_t     busy = 0;
    for (uint64_t d : g_dur)
        busy += d;
    std::fprintf(stderr,
                 "SHIM kernels=%zu dropped=%" PRIu64 " activity_bytes=%" PRIu64
                 " wall_s=%.3f kernel_span_s=%.3f rate_per_s=%.0f gpu_busy_pct=%.2f\n",
                 n, g_dropped.load(), g_bytes.load(), wall_s, span_s, span_s > 0 ? n / span_s : 0.0,
                 span_s > 0 ? 100.0 * double(busy) / (span_s * 1e9) : 0.0);
    if (n)
        std::fprintf(stderr,
                     "SHIM dur_ns p50=%" PRIu64 " p90=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64
                     " min=%" PRIu64 "\n",
                     g_dur[n / 2], g_dur[n * 9 / 10], g_dur[n * 99 / 100], g_dur[n - 1], g_dur[0]);
}

}  // namespace
