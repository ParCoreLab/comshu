// wait_mem: park a thread on an 8-byte word until it may have changed.
//
// Ported from SigmaTrace/Profiler/Modules/MemoryHijack/MemoryRepeater.cpp. The
// three wait instructions each need their own -mtune feature enabled, which is
// why they live in a .cpp with per-function target attributes rather than in the
// header: a translation unit compiled for a baseline x86-64 cannot emit
// UMWAIT/MWAITX inline, but it can call a function tagged with the feature.

#include "memory_repeater.hpp"

#include <cpuid.h>
#include <immintrin.h>
#include <limits>
#include <x86intrin.h>

namespace mlx5::repeater {
namespace {

using WaitFn = void (*)(void*) noexcept;

bool has_mwaitx() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000001u)
        return false;
    __cpuid(0x80000001u, eax, ebx, ecx, edx);
    // MONITORX/MWAITX: CPUID.80000001H:ECX bit 29.
    return (ecx & (1u << 29)) != 0;
}

bool has_waitpkg() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (__get_cpuid_max(0, nullptr) < 7)
        return false;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    // WAITPKG (UMONITOR/UMWAIT/TPAUSE): CPUID.(EAX=7,ECX=0):ECX bit 5.
    return (ecx & (1u << 5)) != 0;
}

__attribute__((target("mwaitx"))) void wait_mwaitx(void* addr) noexcept {
    _mm_monitorx(addr, 0, 0);
    _mm_mwaitx(0, 0, std::numeric_limits<unsigned>::max());
}

__attribute__((target("waitpkg"))) void wait_waitpkg(void* addr) noexcept {
    _umonitor(addr);
    // state 0 = C0.2: deeper sleep, slower wakeup; the doorbell is not latency-
    // critical to the microsecond, and lower power keeps the parked core cool.
    _umwait(0, std::numeric_limits<unsigned>::max());
}

void wait_pause(void*) noexcept {
    _mm_pause();
}

// Chosen once. The fallback is a bare PAUSE, so on a host without either feature
// the repeater is a spin loop per UAR — the cost wait_is_spin() exists to admit.
struct Selected {
    WaitFn fn;
    bool   is_spin;
};

Selected select() noexcept {
    if (has_waitpkg())
        return {wait_waitpkg, false};
    if (has_mwaitx())
        return {wait_mwaitx, false};
    return {wait_pause, true};
}

const Selected& selected() noexcept {
    static const Selected s = select();
    return s;
}

}  // namespace

void wait_mem(void* addr) noexcept {
    selected().fn(addr);
}

bool wait_is_spin() noexcept {
    return selected().is_spin;
}

}  // namespace mlx5::repeater
