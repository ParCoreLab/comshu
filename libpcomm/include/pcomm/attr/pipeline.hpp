#pragma once
// L2 — AttributionPipeline::process. Header-only for the skeleton.

#include "pcomm/attr/interfaces.hpp"
#include "pcomm/core/records.hpp"

namespace pcomm::attr {

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

        for (auto& attributor : m_chain)
            attributor->attribute(a, rec, *m_rec);

        m_rec->record(rec);
    }
}

}  // namespace pcomm::attr
