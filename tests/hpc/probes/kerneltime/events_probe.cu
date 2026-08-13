// PROBE part 3 — can CUDA events give us kernel lifetimes WITHOUT CUPTI?
//
// Why this matters more than it looks. Part 2 measured that NVBit dlopens
// libcupti.so.12 and takes the process's single CUPTI subscriber slot, so
// `cuptiActivityRegisterCallbacks` fails with
// CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED whenever gpumem is loaded.
// If CUPTI were the only way to time a kernel, then "does this capture know
// when kernels ran" would answer "only if you did NOT run NVBit" — the mirror
// image of the bug PLAN-20260810-0505 was written to kill.
//
// cudaEventRecord touches none of that machinery. It is a marker the driver
// puts in the stream and the device timestamps. So if events are accurate
// enough and cheap enough, the ENGINE can own kernel timing unconditionally,
// and CUPTI becomes an optional higher-fidelity source rather than the only one.
//
// This probe runs events and CUPTI over the SAME launches, so CUPTI is the
// ground truth the event numbers are scored against rather than a second
// opinion.
//
// Build:
//   nvcc -arch=sm_90 -O2 -o events_probe events_probe.cu \
//        -I$CUDA_HOME/extras/CUPTI/include \
//        -L$CUDA_HOME/extras/CUPTI/lib64 -lcupti
// Run: ./events_probe [iters] [cycles]

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>
#include <cupti.h>

static inline uint64_t ns_of(clockid_t c) {
    timespec t;
    clock_gettime(c, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec;
}

// ------------------------------------------------------ CUPTI ground truth
struct KernelRec {
    uint64_t start, end;
    uint32_t correlation;
};
static std::vector<KernelRec> g_kernels;
static bool                   g_cupti_on = false;

static void CUPTIAPI on_req(uint8_t** buf, size_t* size, size_t* max_records) {
    void* p = nullptr;
    if (posix_memalign(&p, 8, 8u << 20) != 0)
        p = nullptr;
    *buf = (uint8_t*)p;
    *size = 8u << 20;
    *max_records = 0;
}
static void CUPTIAPI on_done(CUcontext, uint32_t, uint8_t* buf, size_t, size_t valid) {
    CUpti_Activity* rec = nullptr;
    if (!buf)
        return;
    while (cuptiActivityGetNextRecord(buf, valid, &rec) == CUPTI_SUCCESS)
        if (rec->kind == CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL) {
            auto* k = (CUpti_ActivityKernel9*)rec;
            g_kernels.push_back({k->start, k->end, k->correlationId});
        }
    free(buf);
}

__global__ void spin(uint64_t cycles, float* out) {
    long long t0 = clock64();
    float     acc = 0.f;
    while ((unsigned long long)(clock64() - t0) < cycles)
        acc += 1.0f;
    if (threadIdx.x == 1024)
        *out = acc;
}

int main(int argc, char** argv) {
    const int      iters = argc > 1 ? atoi(argv[1]) : 2000;
    const uint64_t cycles = argc > 2 ? strtoull(argv[2], nullptr, 10) : 20000;

    float* d_out = nullptr;
    cudaFree(0);
    cudaMalloc(&d_out, sizeof(float));
    cudaStream_t s;
    cudaStreamCreate(&s);

    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("DEVICE %s\n", prop.name);

    auto workload = [&](bool with_events, std::vector<cudaEvent_t>* ev) {
        const uint64_t t0 = ns_of(CLOCK_MONOTONIC_RAW);
        for (int i = 0; i < iters; i++) {
            if (with_events)
                cudaEventRecord((*ev)[2 * i], s);
            spin<<<1, 128, 0, s>>>(cycles, d_out);
            if (with_events)
                cudaEventRecord((*ev)[2 * i + 1], s);
        }
        cudaStreamSynchronize(s);
        return double(ns_of(CLOCK_MONOTONIC_RAW) - t0) / 1e6;
    };

    // ---- baseline, nothing attached -------------------------------------
    workload(false, nullptr);  // warm
    const double base = workload(false, nullptr);

    // ---- events only, no CUPTI in the process ---------------------------
    // Two events per launch, pre-created: an engine would pool them exactly so,
    // because cudaEventCreate inside the launch path is the thing that would
    // actually cost.
    std::vector<cudaEvent_t> ev(2 * iters);
    for (auto& e : ev)
        cudaEventCreate(&e);
    // An anchor event lets a relative elapsed-time be placed on the wall clock:
    // record it, sync, and read the host clock at a moment the device has
    // definitely reached.
    cudaEvent_t anchor;
    cudaEventCreate(&anchor);
    cudaEventRecord(anchor, s);
    cudaEventSynchronize(anchor);
    const uint64_t anchor_wall = ns_of(CLOCK_REALTIME);

    const double ev_ms = workload(true, &ev);
    std::printf("BASELINE ms=%.3f  EVENTS ms=%.3f  overhead=%.2f%%\n", base, ev_ms,
                100.0 * (ev_ms - base) / base);

    // Durations from events, and their wall-clock placement.
    std::vector<int64_t> ev_dur, ev_start;
    for (int i = 0; i < iters; i++) {
        float ms = 0.f;
        cudaEventElapsedTime(&ms, ev[2 * i], ev[2 * i + 1]);
        ev_dur.push_back(int64_t(double(ms) * 1e6));
        float from_anchor = 0.f;
        cudaEventElapsedTime(&from_anchor, anchor, ev[2 * i]);
        ev_start.push_back(int64_t(anchor_wall) + int64_t(double(from_anchor) * 1e6));
    }

    // ---- same launches, now with CUPTI watching -------------------------
    // CUPTI is enabled AFTER the event-only pass so the event overhead number
    // above is uncontaminated; this pass exists to score events against truth.
    if (cuptiActivityRegisterCallbacks(on_req, on_done) == CUPTI_SUCCESS &&
        cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL) == CUPTI_SUCCESS) {
        g_cupti_on = true;
    } else {
        std::printf("CUPTI unavailable in this process (expected under NVBit)\n");
    }

    std::vector<cudaEvent_t> ev2(2 * iters);
    for (auto& e : ev2)
        cudaEventCreate(&e);
    const double both_ms = workload(true, &ev2);
    cudaStreamSynchronize(s);
    if (g_cupti_on)
        cuptiActivityFlushAll(1);

    std::vector<int64_t> ev2_dur, ev2_start;
    for (int i = 0; i < iters; i++) {
        float ms = 0.f;
        cudaEventElapsedTime(&ms, ev2[2 * i], ev2[2 * i + 1]);
        ev2_dur.push_back(int64_t(double(ms) * 1e6));
        float from_anchor = 0.f;
        cudaEventElapsedTime(&from_anchor, anchor, ev2[2 * i]);
        ev2_start.push_back(int64_t(anchor_wall) + int64_t(double(from_anchor) * 1e6));
    }
    std::printf("BOTH ms=%.3f (events+cupti)\n", both_ms);

    auto pct = [](const char* tag, std::vector<int64_t> v, const char* unit) {
        if (v.empty()) {
            std::printf("%s n=0\n", tag);
            return;
        }
        std::sort(v.begin(), v.end());
        std::printf("%s n=%zu p50=%" PRId64 " p90=%" PRId64 " p99=%" PRId64 " max=%" PRId64 " %s\n",
                    tag, v.size(), v[v.size() / 2], v[v.size() * 9 / 10], v[v.size() * 99 / 100],
                    v.back(), unit);
    };
    pct("EVDUR", ev_dur, "ns (event-measured kernel duration, events only)");

    if (g_cupti_on && !g_kernels.empty()) {
        std::sort(g_kernels.begin(), g_kernels.end(),
                  [](const KernelRec& a, const KernelRec& b) { return a.start < b.start; });
        std::vector<int64_t> cu_dur, d_dur, d_start;
        const size_t         n = std::min(g_kernels.size(), ev2_dur.size());
        for (size_t i = 0; i < n; i++) {
            cu_dur.push_back(int64_t(g_kernels[i].end - g_kernels[i].start));
            int64_t dd = ev2_dur[i] - cu_dur.back();
            d_dur.push_back(dd < 0 ? -dd : dd);
            int64_t ds = ev2_start[i] - int64_t(g_kernels[i].start);
            d_start.push_back(ds < 0 ? -ds : ds);
        }
        pct("CUDUR", cu_dur, "ns (CUPTI kernel duration — ground truth)");
        pct("DELTA_DUR", d_dur, "ns |event duration - CUPTI duration|");
        pct("DELTA_START", d_start, "ns |event wall start - CUPTI wall start|");
        std::printf("CUPTI_KERNELS %zu\n", g_kernels.size());
    }
    return 0;
}
