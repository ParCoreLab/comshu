#pragma once
// L2 — AttributionPipeline::process. Header-only for the skeleton.

#include "comshu/attr/interfaces.hpp"
#include "comshu/core/records.hpp"

namespace comshu::attr {

inline void AttributionPipeline::process(std::span<const RawAccess> accesses) {
    for (const RawAccess& a : accesses) {
        bool kept = true;
        for (auto& f : m_filters) {
            if (!f->keep(a)) {
                kept = false;
                break;
            }
        }
        if (!kept)
            continue;

        core::records::GpuAccess rec{};
        rec.pc = static_cast<core::Handle>(a.pc);
        rec.address = a.address;
        rec.size = a.size;
        rec.op = a.op;
        rec.space = a.space;
        rec.run_device = a.run_device;
        rec.mem_device = a.mem_device;
        rec.thread = a.thread;
        rec.launch = a.launch;

        for (auto& attributor : m_chain)
            attributor->attribute(a, rec, *m_rec);

        // A window, not an instant. The producer knows only that the access
        // happened somewhere in [ticks_lo, ticks_hi] — see RawAccess. Emitting
        // an Instant here (which this did, via the one-argument record(), with
        // the drain-thread's own clock) claimed a precision nothing had, and
        // put a batch's worth of accesses at the same tick in flush order.
        //
        // A producer that supplies no bound gets Temporal::none(): timeless is
        // true, an invented timestamp is not.
        m_rec->record(a.ticks_hi ? core::Temporal::window({0, a.ticks_lo}, {0, a.ticks_hi})
                                 : core::Temporal::none(),
                      rec);
    }
}

}  // namespace comshu::attr
