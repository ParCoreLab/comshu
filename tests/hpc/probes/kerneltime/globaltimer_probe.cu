// PROBE part 4 — is the device `globaltimer` a usable clock domain?
//
// This is the fallback the whole design hinges on when CUPTI is unavailable,
// which part 2 measured is exactly whenever gpumem/NVBit is loaded (NVBit
// dlopens libcupti and takes the process's one subscriber slot). NVBit can
// inject a prologue/epilogue that reads `%globaltimer`, so device-side kernel
// bounds are reachable — but only if that counter can be placed on the same
// wall clock as everything else in a capture.
//
// Two questions:
//   Q1 what IS globaltimer relative to CLOCK_REALTIME — same rate? fixed
//      offset? does it drift?
//   Q2 how expensive is a globaltimer read at kernel entry/exit?
//
// Q1 decides whether a GpuKernel record needs its own clock DOMAIN plus a
// ClockAnchor per GPU (the way _clock already handles per-rank TSC), or whether
// it can be folded into the existing wall-clock domain the way CUPTI's
// timestamps can — part 1 measured CUPTI's timebase IS CLOCK_REALTIME to within
// 162 ns, so it needs no anchoring at all.
//
// Build: nvcc -arch=sm_90 -O2 -o globaltimer_probe globaltimer_probe.cu

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

#include <cuda_runtime.h>

static inline uint64_t ns_of(clockid_t c) {
    timespec t;
    clock_gettime(c, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec;
}

__device__ __forceinline__ uint64_t gtimer() {
    uint64_t t;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
    return t;
}

// Reads globaltimer once, as close to a host clock read as we can arrange.
__global__ void stamp(uint64_t* out) {
    if (threadIdx.x == 0 && blockIdx.x == 0)
        *out = gtimer();
}

// The shape an injected prologue/epilogue would take: one read at entry, one at
// exit, one store each. This is the per-kernel cost NVBit instrumentation would
// add on top of NVBit already being loaded.
__global__ void spin_timed(uint64_t cycles, float* acc_out, uint64_t* t0, uint64_t* t1) {
    if (threadIdx.x == 0)
        t0[blockIdx.x] = gtimer();
    long long s = clock64();
    float     acc = 0.f;
    while ((unsigned long long)(clock64() - s) < cycles)
        acc += 1.0f;
    if (threadIdx.x == 1024)
        *acc_out = acc;
    if (threadIdx.x == 0)
        t1[blockIdx.x] = gtimer();
}

__global__ void spin_plain(uint64_t cycles, float* acc_out) {
    long long s = clock64();
    float     acc = 0.f;
    while ((unsigned long long)(clock64() - s) < cycles)
        acc += 1.0f;
    if (threadIdx.x == 1024)
        *acc_out = acc;
}

int main(int argc, char** argv) {
    const int      iters = argc > 1 ? atoi(argv[1]) : 2000;
    const uint64_t cycles = argc > 2 ? strtoull(argv[2], nullptr, 10) : 20000;

    cudaFree(0);
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("DEVICE %s sm_%d%d\n", prop.name, prop.major, prop.minor);

    uint64_t* d_t = nullptr;
    cudaMalloc(&d_t, sizeof(uint64_t));
    float* d_acc = nullptr;
    cudaMalloc(&d_acc, sizeof(float));

    // ---- Q1: globaltimer vs the host wall clock, twice, seconds apart ----
    // Bracket the device read with two host reads so the host-side uncertainty
    // is visible rather than assumed away.
    auto sample = [&](const char* tag) {
        uint64_t h;
        cudaDeviceSynchronize();
        const uint64_t before = ns_of(CLOCK_REALTIME);
        stamp<<<1, 1>>>(d_t);
        cudaDeviceSynchronize();
        const uint64_t after = ns_of(CLOCK_REALTIME);
        cudaMemcpy(&h, d_t, sizeof(h), cudaMemcpyDeviceToHost);
        const uint64_t mid = before + (after - before) / 2;
        std::printf("GT %s globaltimer=%" PRIu64 " wall_mid=%" PRIu64 " delta=%+" PRId64
                    " host_window=%" PRIu64 " mono=%" PRIu64 "\n",
                    tag, h, mid, int64_t(h) - int64_t(mid), after - before,
                    ns_of(CLOCK_MONOTONIC_RAW));
        return std::pair<uint64_t, uint64_t>{h, mid};
    };
    auto            s0 = sample("t0");
    struct timespec rq{3, 0};
    nanosleep(&rq, nullptr);
    auto s1 = sample("t1");

    const double dev_elapsed = double(s1.first - s0.first);
    const double host_elapsed = double(s1.second - s0.second);
    std::printf("GT_RATE dev_ns=%.0f host_ns=%.0f ratio=%.9f drift_ppm=%.3f\n", dev_elapsed,
                host_elapsed, dev_elapsed / host_elapsed, 1e6 * (dev_elapsed / host_elapsed - 1.0));

    // ---- Q2: what does the instrumentation cost? -------------------------
    uint64_t *d_a = nullptr, *d_b = nullptr;
    cudaMalloc(&d_a, sizeof(uint64_t) * 64);
    cudaMalloc(&d_b, sizeof(uint64_t) * 64);

    auto bench = [&](bool timed) {
        for (int i = 0; i < 200; i++)  // warm
            timed ? spin_timed<<<1, 128>>>(cycles, d_acc, d_a, d_b)
                  : spin_plain<<<1, 128>>>(cycles, d_acc);
        cudaDeviceSynchronize();
        const uint64_t t0 = ns_of(CLOCK_MONOTONIC_RAW);
        for (int i = 0; i < iters; i++)
            timed ? spin_timed<<<1, 128>>>(cycles, d_acc, d_a, d_b)
                  : spin_plain<<<1, 128>>>(cycles, d_acc);
        cudaDeviceSynchronize();
        return double(ns_of(CLOCK_MONOTONIC_RAW) - t0) / 1e6;
    };
    const double plain = bench(false);
    const double timed = bench(true);
    std::printf("GT_COST plain_ms=%.3f timed_ms=%.3f overhead=%.2f%% per_kernel_ns=%.1f\n", plain,
                timed, 100.0 * (timed - plain) / plain, 1e6 * (timed - plain) / iters);

    // What the injected pair actually measures, for comparison with CUPTI's
    // duration on the identical workload.
    uint64_t a = 0, b = 0;
    cudaMemcpy(&a, d_a, sizeof(a), cudaMemcpyDeviceToHost);
    cudaMemcpy(&b, d_b, sizeof(b), cudaMemcpyDeviceToHost);
    std::printf("GT_DUR last_kernel_ns=%" PRIu64 "\n", b - a);
    return 0;
}
