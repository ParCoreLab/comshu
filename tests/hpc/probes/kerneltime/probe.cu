// PROBE — how should pcomm capture kernel lifetimes? See README.md.
//
// Four questions, one binary, because they share a workload and the answers
// only mean anything together:
//
//   Q1 does CUPTI's concurrent-kernel activity work here at all, unprivileged?
//   Q2 what clock are CUPTI timestamps in, and can a host TSC reach it?
//   Q3 how far apart are SUBMISSION and EXECUTION? (this is the whole
//      host-interposer-vs-CUPTI decision, in one number)
//   Q4 what does CUPTI cost?
//
// Build:
//   nvcc -arch=sm_90 -O2 -o probe probe.cu \
//        -I$CUDA_HOME/extras/CUPTI/include \
//        -L$CUDA_HOME/extras/CUPTI/lib64 -lcupti
//
// Run: ./probe [iters]     (default 2000)

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>
#include <cupti.h>

// ---------------------------------------------------------------- clocks
static inline uint64_t rdtsc() { return __builtin_ia32_rdtsc(); }
static inline uint64_t ns_of(clockid_t c) {
    timespec t;
    clock_gettime(c, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec;
}

#define CUPTI_CHECK(x)                                                                     \
    do {                                                                                   \
        CUptiResult _r = (x);                                                              \
        if (_r != CUPTI_SUCCESS) {                                                         \
            const char* _m = nullptr;                                                      \
            cuptiGetResultString(_r, &_m);                                                 \
            std::printf("CUPTI_FAIL %s -> %s\n", #x, _m ? _m : "?");                       \
            return -1;                                                                     \
        }                                                                                  \
    } while (0)

// ------------------------------------------------------- activity plumbing
static constexpr size_t kBufSize = 8u << 20;
static constexpr size_t kAlign = 8;

struct KernelRec {
    uint64_t start, end;
    uint32_t correlation;
    uint32_t device;
    uint32_t stream;
};
struct ApiRec {
    uint64_t start, end;
    uint32_t correlation;
};

static std::vector<KernelRec> g_kernels;
static std::vector<ApiRec>    g_api;
static uint64_t               g_dropped = 0;

static void CUPTIAPI on_buffer_request(uint8_t** buf, size_t* size, size_t* max_records) {
    // Deliberately not aligned_alloc'd into a pool: this probe measures CUPTI's
    // cost, not our allocator's.
    void* p = nullptr;
    if (posix_memalign(&p, kAlign, kBufSize) != 0)
        p = nullptr;
    *buf = (uint8_t*)p;
    *size = kBufSize;
    *max_records = 0;
}

static void CUPTIAPI on_buffer_complete(CUcontext, uint32_t, uint8_t* buf, size_t, size_t valid) {
    CUpti_Activity* rec = nullptr;
    if (buf) {
        while (cuptiActivityGetNextRecord(buf, valid, &rec) == CUPTI_SUCCESS) {
            if (rec->kind == CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL) {
                auto* k = (CUpti_ActivityKernel9*)rec;
                g_kernels.push_back({k->start, k->end, k->correlationId,
                                     (uint32_t)k->deviceId, (uint32_t)k->streamId});
            } else if (rec->kind == CUPTI_ACTIVITY_KIND_RUNTIME) {
                auto* a = (CUpti_ActivityAPI*)rec;
                g_api.push_back({a->start, a->end, a->correlationId});
            }
        }
        size_t dropped = 0;
        cuptiActivityGetNumDroppedRecords(nullptr, 0, &dropped);
        g_dropped += dropped;
        free(buf);
    }
}

// ------------------------------------------------------------- workload
__global__ void spin(uint64_t cycles, float* out) {
    // A kernel with a controllable, non-trivial duration. clock64() rather than
    // a loop count so the duration is roughly stable across clock states.
    long long t0 = clock64();
    float     acc = 0.f;
    while ((unsigned long long)(clock64() - t0) < cycles)
        acc += 1.0f;
    if (threadIdx.x == 1024)  // never true; keeps acc live
        *out = acc;
}

struct HostStamp {
    uint64_t tsc_before, tsc_after;
};
static std::vector<HostStamp> g_host;

// Run `iters` launches. `sync` picks the two regimes that matter:
//   true  — launch, synchronize, launch. Submission ~= execution.
//   false — a deep async queue, which is what a real comm workload does.
static double run_workload(int iters, uint64_t cycles, bool sync, float* d_out,
                           cudaStream_t s, bool stamp) {
    const uint64_t t0 = ns_of(CLOCK_MONOTONIC_RAW);
    for (int i = 0; i < iters; i++) {
        uint64_t a = stamp ? rdtsc() : 0;
        spin<<<1, 128, 0, s>>>(cycles, d_out);
        uint64_t b = stamp ? rdtsc() : 0;
        if (stamp)
            g_host.push_back({a, b});
        if (sync)
            cudaStreamSynchronize(s);
    }
    cudaStreamSynchronize(s);
    return double(ns_of(CLOCK_MONOTONIC_RAW) - t0) / 1e6;  // ms
}

// --------------------------------------------------------- Q2: the clocks
// Sample cuptiGetTimestamp beside every host clock we might anchor to. Twice,
// seconds apart, so slope (drift) is visible and not just offset.
static void sample_clocks(const char* tag) {
    uint64_t c = 0;
    cuptiGetTimestamp(&c);
    const uint64_t real = ns_of(CLOCK_REALTIME);
    const uint64_t mono = ns_of(CLOCK_MONOTONIC_RAW);
    const uint64_t boot = ns_of(CLOCK_BOOTTIME);
    const uint64_t tsc = rdtsc();
    std::printf("CLOCKS %s cupti=%" PRIu64 " real=%" PRIu64 " mono=%" PRIu64 " boot=%" PRIu64
                " tsc=%" PRIu64 "\n",
                tag, c, real, mono, boot, tsc);
    std::printf("CLOCKD %s cupti-mono=%+" PRId64 " cupti-real=%+" PRId64 " cupti-boot=%+" PRId64
                "\n",
                tag, int64_t(c - mono), int64_t(c - real), int64_t(c - boot));
}

int main(int argc, char** argv) {
    const int iters = argc > 1 ? atoi(argv[1]) : 2000;
    const uint64_t cycles = argc > 2 ? strtoull(argv[2], nullptr, 10) : 20000;

    float* d_out = nullptr;
    cudaMalloc(&d_out, sizeof(float));
    cudaFree(0);  // force context creation before any timing
    cudaStream_t s;
    cudaStreamCreate(&s);

    int dev = 0;
    cudaGetDevice(&dev);
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, dev);
    std::printf("DEVICE %s sm_%d%d\n", prop.name, prop.major, prop.minor);

    // ---- Q4a: baseline, no CUPTI in the process at all -------------------
    run_workload(200, cycles, false, d_out, s, false);  // warm
    const double base_async = run_workload(iters, cycles, false, d_out, s, false);
    const double base_sync = run_workload(iters, cycles, true, d_out, s, false);
    std::printf("BASELINE async_ms=%.3f sync_ms=%.3f iters=%d\n", base_async, base_sync, iters);

    // ---- Q1: can we turn CUPTI on, unprivileged? -------------------------
    CUPTI_CHECK(cuptiActivityRegisterCallbacks(on_buffer_request, on_buffer_complete));
    CUPTI_CHECK(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
    CUPTI_CHECK(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_RUNTIME));
    std::printf("CUPTI_ENABLE ok\n");

    sample_clocks("t0");

    // ---- Q3 + Q4b: the same two regimes, now measured --------------------
    g_host.clear();
    const double cupti_async = run_workload(iters, cycles, false, d_out, s, true);
    const size_t async_host = g_host.size();
    const double cupti_sync = run_workload(iters, cycles, true, d_out, s, true);

    cuptiActivityFlushAll(1);
    sample_clocks("t1");

    std::printf("CUPTI_ON async_ms=%.3f sync_ms=%.3f\n", cupti_async, cupti_sync);
    std::printf("OVERHEAD async=%.2f%% sync=%.2f%%\n",
                100.0 * (cupti_async - base_async) / base_async,
                100.0 * (cupti_sync - base_sync) / base_sync);
    std::printf("RECORDS kernels=%zu api=%zu dropped=%" PRIu64 "\n", g_kernels.size(),
                g_api.size(), g_dropped);

    // ---- Q3: submission vs execution, correlated -------------------------
    // The RUNTIME record is the launch API call; the KERNEL record is the
    // execution. Same correlationId, same timebase — so the difference is the
    // exact quantity a host-side interposer would get WRONG.
    std::map<uint32_t, ApiRec> api_by_corr;
    for (const auto& a : g_api)
        api_by_corr[a.correlation] = a;

    std::vector<int64_t> skew_async, skew_sync;
    size_t               matched = 0;
    // Launches are appended in order; the first `async_host` correlate to the
    // async regime. Ordering by kernel start is enough to split them, since the
    // regimes are sequential in time.
    std::vector<KernelRec> ks = g_kernels;
    std::sort(ks.begin(), ks.end(), [](const KernelRec& a, const KernelRec& b) {
        return a.start < b.start;
    });
    for (size_t i = 0; i < ks.size(); i++) {
        auto it = api_by_corr.find(ks[i].correlation);
        if (it == api_by_corr.end())
            continue;
        matched++;
        const int64_t d = int64_t(ks[i].start) - int64_t(it->second.start);
        (i < async_host ? skew_async : skew_sync).push_back(d);
    }
    auto report = [](const char* tag, std::vector<int64_t>& v) {
        if (v.empty()) {
            std::printf("SKEW %s n=0\n", tag);
            return;
        }
        std::sort(v.begin(), v.end());
        std::printf("SKEW %s n=%zu p50=%" PRId64 " p90=%" PRId64 " p99=%" PRId64 " max=%" PRId64
                    " (ns, kernel.start - launch_api.start)\n",
                    tag, v.size(), v[v.size() / 2], v[v.size() * 9 / 10], v[v.size() * 99 / 100],
                    v.back());
    };
    std::printf("MATCHED %zu of %zu kernels to a launch API record\n", matched, ks.size());
    report("async", skew_async);
    report("sync", skew_sync);

    // Kernel durations, for the record: this is what a timeline would draw.
    std::vector<int64_t> dur;
    for (const auto& k : ks)
        dur.push_back(int64_t(k.end - k.start));
    report("duration", dur);

    cuptiActivityFlushAll(1);
    cudaFree(d_out);
    return 0;
}
