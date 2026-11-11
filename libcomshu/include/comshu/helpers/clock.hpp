#pragma once
// Helpers — timestamp sources (Strategy; selected via Recorder Config).
// Raw local ticks only: conversion/alignment is comshu-read's job
// (UNIFICATION §4). TSC calibration generalizes SigmaTrace's TscCalibration.

#include <cstdint>
#include <ctime>

#include "comshu/core/ids.hpp"

namespace comshu::helpers {

// x86 TSC when available; CLOCK_MONOTONIC_RAW ns otherwise. Either way the
// value is an opaque tick count tied to a clock domain.
struct TscClock {
    static core::Ticks now() {
#if defined(__x86_64__)
        return __builtin_ia32_rdtsc();
#else
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return static_cast<core::Ticks>(ts.tv_sec) * 1'000'000'000ull + ts.tv_nsec;
#endif
    }
};

// Snapshot for a ClockAnchor artifact (start/end of run, per domain).
struct AnchorSample {
    core::Ticks   local_ticks;
    std::uint64_t wall_ns;
    std::uint64_t mono_ns;
};

template<class Clock = TscClock> inline AnchorSample sample_anchor() {
    timespec wall, mono;
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_MONOTONIC_RAW, &mono);
    return {
        .local_ticks = Clock::now(),
        .wall_ns = static_cast<std::uint64_t>(wall.tv_sec) * 1'000'000'000ull + wall.tv_nsec,
        .mono_ns = static_cast<std::uint64_t>(mono.tv_sec) * 1'000'000'000ull + mono.tv_nsec,
    };
}

}  // namespace comshu::helpers
