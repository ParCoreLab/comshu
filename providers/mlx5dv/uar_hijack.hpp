#pragma once
// The UAR doorbell hijack — Stage 1: the swap, as a pure forwarder.
//
// Ported from SigmaTrace/Profiler/Handlers/CUDAHijack.cpp. Hooks the CUDA DRIVER
// API (cuMemHostRegister_v2 / cuMemHostGetDevicePointer_v2 / cuMemHostUnregister),
// reached through the export-table patcher in cuda_export_table.hpp — because on
// du04 NVSHMEM links libcudart statically and cudart_static calls libcuda's
// cuMemHostGetDevicePointer through the driver export table, not through any
// dynamic symbol or cuGetProcAddress. See cuda_export_table.hpp for the evidence.
//
// The mechanism, in the order the pieces exist:
//
//   0. mlx5dv_devx_alloc_uar hands out a doorbell page. The provider records its
//      reg_addr and calls remember_uar() here.
//   1. cudaHostRegister(reg_addr, …, Mapped) -> cudart_static -> cuMemHostRegister.
//      We recognise the UAR and SKIP the real registration — the real BAR page is
//      never mapped to the GPU; only the CPU (this hijack's repeater) writes it.
//   2. cudaHostGetDevicePointer(&ptr, reg_addr, …) -> cuMemHostGetDevicePointer.
//      THE SWAP: allocate a fake host page, park a repeater on it, and return the
//      fake page's device pointer. The GPU rings the fake page; the repeater
//      forwards each store to the real UAR, so the NIC still sends.
//
// Stage 1 attaches no decode: the repeater callback only counts, so a full
// NVSHMEM run can be measured for byte-identical output and overhead inside the
// noise of three clean runs. If that gate fails, no amount of correct parsing
// rescues a data path that perturbs the application, and the work stops here.
//
// THREE RULES, because this sits in the data path where every other pcomm hook
// is passive:
//   * Fail open, not closed. Cannot allocate the fake page, cannot resolve a
//     driver call, no UAR match -> return the REAL device pointer (registering
//     the page lazily, since step 1 skipped it) and count the UAR as unwatched.
//   * Never manufacture a capability. Only pages mlx5dv_devx_alloc_uar handed
//     out are swapped; everything else passes straight through.
//   * Forward unconditionally. That rule lives in memory_repeater.hpp, on the
//     store the repeater must never skip.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cuda_export_table.hpp"
#include "memory_repeater.hpp"

namespace mlx5::hijack {

// CUDA driver types and flags, hand-copied from cuda.h and checked by nothing —
// the same exposure the rest of this provider carries, so it builds where CUDA
// does not. CUresult is an int; CUdeviceptr is 64-bit on LP64.
using CuResult = int;
using CuDevicePtr = unsigned long long;
inline constexpr CuResult kCudaSuccess = 0;
inline constexpr CuResult kCudaErrorInvalidValue = 1;

inline constexpr unsigned    kMemHostRegisterDeviceMap = 0x02;  // CU_MEMHOSTREGISTER_DEVICEMAP
inline constexpr unsigned    kMemHostAllocPortable = 0x01;      // CU_MEMHOSTALLOC_PORTABLE
inline constexpr unsigned    kMemHostAllocDeviceMap = 0x02;     // CU_MEMHOSTALLOC_DEVICEMAP
inline constexpr std::size_t kUarPageSize = 4096;

using PfnHostRegister = CuResult (*)(void*, std::size_t, unsigned);
using PfnHostGetDevicePointer = CuResult (*)(CuDevicePtr*, void*, unsigned);
using PfnHostUnregister = CuResult (*)(void*);
using PfnHostAlloc = CuResult (*)(void**, std::size_t, unsigned);
using PfnFreeHost = CuResult (*)(void*);

// The launcher's --enable-doorbell-hijack sets PCOMM_DOORBELL_HIJACK=1. Checked
// once; gates whether the provider installs the export-table patcher at all.
inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_DOORBELL_HIJACK");
        return v && *v == '1';
    }();
    return on;
}

inline bool debug_on() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_HIJACK_DEBUG");
        return v && *v == '1';
    }();
    return on;
}

// --- real libcuda driver functions -----------------------------------------
// Resolved through the export-table module: get_real returns the pointer it
// captured while patching, or resolves by name from the promoted-global libcuda.
inline PfnHostRegister real_host_register() {
    return reinterpret_cast<PfnHostRegister>(export_table::real("cuMemHostRegister_v2"));
}
inline PfnHostGetDevicePointer real_host_get_device_pointer() {
    return reinterpret_cast<PfnHostGetDevicePointer>(
        export_table::real("cuMemHostGetDevicePointer_v2"));
}
inline PfnHostAlloc real_host_alloc() {
    return reinterpret_cast<PfnHostAlloc>(export_table::real("cuMemHostAlloc"));
}
inline PfnFreeHost real_free_host() {
    return reinterpret_cast<PfnFreeHost>(export_table::real("cuMemFreeHost"));
}

struct RegisterIntent {
    std::size_t bytesize{0};
    unsigned    flags{0};
};

struct State {
    std::mutex mutex;

    std::unordered_map<void*, std::uint32_t>          uar_pages;   // reg_addr -> page_id
    std::unordered_map<void*, RegisterIntent>         skipped;     // reg_addr -> skipped register
    std::unordered_map<void*, void*>                  fake_pages;  // reg_addr -> fake page
    std::unordered_map<void*, std::vector<repeater::RepeaterHandle>> handles;  // reg_addr -> repeaters

    std::atomic<std::uint64_t> num_fake_accesses{0};
    std::atomic<std::uint32_t> uars_hijacked{0};
    std::atomic<std::uint32_t> uars_unwatched{0};
    std::atomic<std::uint32_t> remembered_uars{0};
    std::atomic<std::uint32_t> register_calls{0};
    std::atomic<std::uint32_t> getdevptr_calls{0};
    std::atomic<std::uint32_t> unregister_calls{0};
};

inline State& state() {
    static State s;
    return s;
}

inline void remember_uar(void* reg_addr, std::uint32_t page_id) {
    if (!reg_addr)
        return;
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        s.uar_pages[reg_addr] = page_id;
    }
    s.remembered_uars.fetch_add(1, std::memory_order_relaxed);
    if (debug_on())
        std::fprintf(stderr, "[hijack] remember_uar reg_addr=%p page_id=%u\n", reg_addr, page_id);
}

// Tear down the repeater(s) for a UAR and free its fake page. Idempotent and
// keyed on reg_addr, because free_uar and cuMemHostUnregister can arrive in
// either order and both call this. Destroying a RepeaterHandle stops delivery,
// stops the thread and joins it; the fake page is freed only after the join.
inline void teardown(void* reg_addr) {
    auto& s = state();

    std::vector<repeater::RepeaterHandle> to_join;
    void*                                 fake = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (auto it = s.handles.find(reg_addr); it != s.handles.end()) {
            to_join = std::move(it->second);
            s.handles.erase(it);
        }
        if (auto it = s.fake_pages.find(reg_addr); it != s.fake_pages.end()) {
            fake = it->second;
            s.fake_pages.erase(it);
        }
    }

    to_join.clear();  // joins every repeater thread before the page is freed

    if (fake)
        if (auto* free_host = real_free_host())
            free_host(fake);
}

// --- the three overwrites, installed into the driver export table ----------

inline CuResult intercept_host_register(void* p, std::size_t bytesize, unsigned flags) {
    auto* real = real_host_register();

    auto& s = state();
    s.register_calls.fetch_add(1, std::memory_order_relaxed);
    if ((flags & kMemHostRegisterDeviceMap) != 0) {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (s.uar_pages.count(p)) {
            s.skipped[p] = RegisterIntent{bytesize, flags};
            if (debug_on())
                std::fprintf(stderr, "[hijack] cuMemHostRegister UAR p=%p flags=0x%x -> SKIP real\n",
                             p, flags);
            return kCudaSuccess;
        }
    }
    if (debug_on())
        std::fprintf(stderr, "[hijack] cuMemHostRegister p=%p flags=0x%x -> passthrough\n", p, flags);
    return real ? real(p, bytesize, flags) : kCudaErrorInvalidValue;
}

inline CuResult intercept_host_get_device_pointer(CuDevicePtr* pdptr, void* p, unsigned flags) {
    auto* real = real_host_get_device_pointer();
    if (!real) {
        if (debug_on())
            std::fprintf(stderr, "[hijack] cuMemHostGetDevicePointer p=%p NO REAL FN\n", p);
        return kCudaErrorInvalidValue;
    }

    auto& s = state();
    s.getdevptr_calls.fetch_add(1, std::memory_order_relaxed);

    RegisterIntent intent;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        auto                        skip_it = s.skipped.find(p);
        if (skip_it == s.skipped.end()) {
            if (debug_on())
                std::fprintf(stderr,
                             "[hijack] cuMemHostGetDevicePointer p=%p -> passthrough (not UAR)\n", p);
            return real(pdptr, p, flags);  // not a UAR page we own — pass through
        }
        intent = skip_it->second;
        // Already swapped: hand back the same fake page's device pointer.
        if (auto fk = s.fake_pages.find(p); fk != s.fake_pages.end())
            return real(pdptr, fk->second, flags);
    }

    // Fail-open: give the application a REAL, working device pointer. Step 1
    // skipped the registration, so register it now, then resolve it directly.
    auto fail_open = [&]() -> CuResult {
        s.uars_unwatched.fetch_add(1, std::memory_order_relaxed);
        if (debug_on())
            std::fprintf(stderr, "[hijack] p=%p FAIL-OPEN -> real register+getdevptr\n", p);
        if (auto* reg = real_host_register())
            reg(p, intent.bytesize, intent.flags);
        return real(pdptr, p, flags);
    };

    // Allocate the fake page.
    auto* host_alloc = real_host_alloc();
    if (!host_alloc)
        return fail_open();
    void*          fake = nullptr;
    const CuResult rc =
        host_alloc(&fake, kUarPageSize, kMemHostAllocPortable | kMemHostAllocDeviceMap);
    if (rc != kCudaSuccess || !fake)
        return fail_open();
    std::memset(fake, 0, kUarPageSize);

    // Park a repeater on the fake page. Stage 1 callback: count only.
    repeater::RepeaterHandle handle(static_cast<std::uint64_t*>(fake), p, [](std::uint64_t) {
        state().num_fake_accesses.fetch_add(1, std::memory_order_relaxed);
    });
    handle.start();

    {
        std::lock_guard<std::mutex> lk(s.mutex);
        s.fake_pages[p] = fake;
        s.handles[p].emplace_back(std::move(handle));
    }
    s.uars_hijacked.fetch_add(1, std::memory_order_relaxed);
    if (debug_on())
        std::fprintf(stderr, "[hijack] SWAP p=%p -> fake=%p (repeater armed)\n", p, fake);

    return real(pdptr, fake, flags);
}

inline CuResult intercept_host_unregister(void* p) {
    auto* real = reinterpret_cast<PfnHostUnregister>(export_table::real("cuMemHostUnregister"));

    auto& s = state();
    s.unregister_calls.fetch_add(1, std::memory_order_relaxed);
    bool was_uar = false;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        was_uar = s.skipped.erase(p) > 0;
    }
    if (was_uar) {
        teardown(p);  // we skipped the register, so skip the unregister too
        return kCudaSuccess;
    }
    return real ? real(p) : kCudaErrorInvalidValue;
}

// Register the three overwrites with the export-table patcher and arm it. Call
// once from the provider constructor when enabled.
inline void install() {
    export_table::register_overwrite("cuMemHostRegister_v2",
                                     reinterpret_cast<void*>(&intercept_host_register));
    export_table::register_overwrite("cuMemHostGetDevicePointer_v2",
                                     reinterpret_cast<void*>(&intercept_host_get_device_pointer));
    export_table::register_overwrite("cuMemHostUnregister",
                                     reinterpret_cast<void*>(&intercept_host_unregister));
    export_table::install();
}

// Free repeater threads and pages still standing at shutdown, and report. The
// counts are the Stage-1 gate's evidence: num_fake_accesses > 0 means the GPU
// rang a page we owned and the man-in-the-middle engaged.
inline void report_and_shutdown(int rank) {
    auto& s = state();

    std::vector<void*> live;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        live.reserve(s.handles.size());
        for (auto& [addr, _] : s.handles)
            live.push_back(addr);
    }
    for (void* addr : live)
        teardown(addr);

    std::fprintf(stderr,
                 "[pcomm rank %d] doorbell hijack: uars_hijacked=%u unwatched=%u "
                 "fake_accesses=%llu | remembered_uars=%u register_calls=%u "
                 "getdevptr_calls=%u unregister_calls=%u%s\n",
                 rank, s.uars_hijacked.load(), s.uars_unwatched.load(),
                 static_cast<unsigned long long>(s.num_fake_accesses.load()),
                 s.remembered_uars.load(), s.register_calls.load(), s.getdevptr_calls.load(),
                 s.unregister_calls.load(),
                 repeater::wait_is_spin() ? " (PAUSE spin — no WAITPKG/MWAITX)" : "");
}

}  // namespace mlx5::hijack
