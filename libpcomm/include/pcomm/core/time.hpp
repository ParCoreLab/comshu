#pragma once
// L0 — the time model. Raw local ticks + clock-domain id on the hot path;
// resolution to a global timeline happens post-fact in pcomm-read.
// See ../UNIFICATION.md §4.

#include <cstdint>
#include <type_traits>

#include "pcomm/core/ids.hpp"

namespace pcomm::core {

enum class TimeKind : std::uint8_t {
    None,      // atemporal (artifacts, purely logical entities)
    Instant,   // a
    Interval,  // [a, b]
    Window,    // known only within [a, b] (uncertainty made explicit)
    Counter,   // value sampled at a
};

// The padding is explicit and value-initialised on purpose. With an implicit
// hole, assigning one of these copies the object representation — padding
// included — so uninitialised stack bytes reach the capture file and stay
// there. Measured before this was made explicit: 5000/5000 records in a real
// capture carried stack residue, with recognisable pointer fragments. Zeroing
// the destination slot does not help; the source is what is dirty.
//
// Layout and size are unchanged (16 B), and this is exactly how
// libpcomm/schema/pcomm.fbs describes it.
struct TimePoint {
    DomainId     domain{0};
    std::uint8_t _pad[6]{};  // keep: see above
    Ticks        ticks{0};

    // The constructor exists so the explicit padding member does not change
    // how callers write these: `{domain, ticks}` still works. Still
    // standard-layout and trivially copyable, which the recorder relies on.
    TimePoint() = default;
    TimePoint(DomainId d, Ticks t) : domain(d), _pad{}, ticks(t) {}
};
static_assert(std::is_trivially_copyable_v<TimePoint>);
static_assert(std::is_standard_layout_v<TimePoint>);

// Attached by the recorder to Events and Entity lifetimes; never stored
// pre-converted — alignment is a reader pass, raw streams are immutable.
struct Temporal {
    TimeKind      kind{TimeKind::None};
    std::uint8_t  _pad[7]{};  // explicit + zeroed, see TimePoint
    TimePoint     a{};        // instant / interval.start / window.lo / counter.t / lifetime.from
    TimePoint     b{};        // interval.end / window.hi / lifetime.to (open if zeroed)
    std::uint64_t value{0};   // counter value

    static Temporal none() { return {}; }
    static Temporal instant(TimePoint t) { return {TimeKind::Instant, {}, t, {}, 0}; }
    static Temporal interval(TimePoint a, TimePoint b) { return {TimeKind::Interval, {}, a, b, 0}; }
    static Temporal window(TimePoint lo, TimePoint hi) { return {TimeKind::Window, {}, lo, hi, 0}; }
    static Temporal counter(TimePoint t, std::uint64_t v) {
        return {TimeKind::Counter, {}, t, {}, v};
    }
};

static_assert(sizeof(Temporal) == 48);
static_assert(sizeof(TimePoint) == 16);  // revisit if we pack for the on-disk stream

}  // namespace pcomm::core
