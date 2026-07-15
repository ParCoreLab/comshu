#pragma once
// L0 — the time model. Raw local ticks + clock-domain id on the hot path;
// resolution to a global timeline happens post-fact in pcomm-read.
// See ../UNIFICATION.md §4.

#include <cstdint>

#include "pcomm/core/ids.hpp"

namespace pcomm::core {

enum class TimeKind : std::uint8_t {
    None,      // atemporal (artifacts, purely logical entities)
    Instant,   // a
    Interval,  // [a, b]
    Window,    // known only within [a, b] (uncertainty made explicit)
    Counter,   // value sampled at a
};

struct TimePoint {
    DomainId domain{0};
    Ticks    ticks{0};
};

// Attached by the recorder to Events and Entity lifetimes; never stored
// pre-converted — alignment is a reader pass, raw streams are immutable.
struct Temporal {
    TimeKind      kind{TimeKind::None};
    TimePoint     a{};        // instant / interval.start / window.lo / counter.t / lifetime.from
    TimePoint     b{};        // interval.end / window.hi / lifetime.to (open if zeroed)
    std::uint64_t value{0};   // counter value

    static Temporal none() { return {}; }
    static Temporal instant(TimePoint t) { return {TimeKind::Instant, t, {}, 0}; }
    static Temporal interval(TimePoint a, TimePoint b) { return {TimeKind::Interval, a, b, 0}; }
    static Temporal window(TimePoint lo, TimePoint hi) { return {TimeKind::Window, lo, hi, 0}; }
    static Temporal counter(TimePoint t, std::uint64_t v) { return {TimeKind::Counter, t, {}, v}; }
};

static_assert(sizeof(Temporal) == 48);  // revisit if we pack for the on-disk stream

}  // namespace pcomm::core
