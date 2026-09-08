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

// How long a parked wait may sleep before it re-checks the word itself.
//
// It is a BOUND, not a delay: a store to the monitored line wakes the core
// immediately, so this only decides how long a *missed* wakeup can go unnoticed.
// ~100k TSC cycles is about 40 us on this class of part — short enough that a
// lost store costs one barrier's worth of latency rather than the run, long
// enough that the core genuinely parks between doorbells instead of spinning.
inline constexpr unsigned long long kWaitCycles = 100000;

__attribute__((target("mwaitx"))) void wait_mwaitx(void* addr) noexcept {
    _mm_monitorx(addr, 0, 0);
    // extensions = 2 sets the timer-enable bit; without it MWAITX ignores the
    // cycle count entirely and waits forever on a store that may already have
    // happened. Passing a count with no enable bit, as this did, is the AMD
    // half of the UMWAIT bug below.
    _mm_mwaitx(2, 0, static_cast<unsigned>(kWaitCycles));
}

__attribute__((target("waitpkg"))) void wait_waitpkg(void* addr) noexcept {
    _umonitor(addr);
    // state 0 = C0.2: deeper sleep, slower wakeup; the doorbell is not latency-
    // critical to the microsecond, and lower power keeps the parked core cool.
    //
    // THE SECOND ARGUMENT IS AN ABSOLUTE TSC DEADLINE, NOT A DURATION, and this
    // used to pass UINT_MAX — 4.29e9, which the TSC passes about 1.6 seconds
    // after boot on a 2.6 GHz part. So the deadline was ALWAYS in the past and
    // UMWAIT returned immediately, every time, on every machine with WAITPKG.
    // The repeater was a hot spin loop wearing a power-management comment.
    //
    // That is not merely wasteful, because the repeater threads inherit the
    // application's CPU affinity. Under `mpirun` with its default --bind-to
    // core, a rank owns one core; measured on MN5 (job 45587015) the main
    // thread and three repeaters were all pinned to psr 80 taking ~27% each,
    // and NVSHMEM's own proxy thread — the thread that has to ring the doorbell
    // under the CPU NIC handler — was starved among them. A workload whose
    // barrier spins on the GPU waiting for that doorbell then never finishes:
    // two-node putget hung indefinitely while shmem_put_bw, which does not park
    // the GPU on a device-side barrier, completed fine.
    //
    // rdtsc is read per wait rather than hoisted: the deadline has to be
    // relative to now, which is the whole point of the fix.
    _umwait(0, __rdtsc() + kWaitCycles);
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
