#pragma once
// L0 — concrete record types for phase 1 (gpumem 700-range + shared kinds).
// One example of each archetype exists from day one so the writer/reader
// handle all four paths. Network records (100s) are ported with the ibverbs
// provider; more gpumem records land with the Snoopie port.

#include <cstdint>

#include "pcomm/core/ids.hpp"
#include "pcomm/core/record.hpp"
#include "pcomm/core/vocab.hpp"

namespace pcomm::core::records {

// EVENT — one GPU memory access (the firehose). snoopie's per-access row,
// normalized: high-cardinality fields are interned Handles; the object link
// is inline (hot 1:1 edge), richer relations become Edge rows.
PCOMM_RECORD(GpuAccess, RecordKind::Event, 700, "gpu_access") {
    Handle        pc;          // interned program counter
    Handle        object;      // interned mem-object id (0 if unattributed)
    std::uint64_t address;
    std::uint32_t size;
    MemOp         op;
    MemSpace      space;
    DeviceId      run_device;  // where the SM ran
    DeviceId      mem_device;  // where the memory lives
    std::uint32_t thread;
};

// ENTITY — a memory object with a lifetime [alloc, free).
PCOMM_RECORD(MemObject, RecordKind::Entity, 720, "mem_object") {
    Handle   id;
    Handle   name;             // interned symbol/type name
    MemRange range;
    DeviceId device;
};

// ARTIFACT — one PC -> source location row (timeless, computed once).
PCOMM_RECORD(SourceLine, RecordKind::Artifact, 740, "source_line") {
    Handle        pc;
    Handle        file;        // interned path
    std::uint32_t line;
};

// EDGE — generic attribution row for non-inline relations.
PCOMM_RECORD(Attribution, RecordKind::Edge, 760, "attribution") {
    Handle        subject;
    Handle        object;
    EdgePredicate pred;
};

// ARTIFACT — clock-domain calibration anchor (UNIFICATION §4). Emitted at
// start/end per domain so pcomm-read can fit offset + skew.
PCOMM_RECORD(ClockAnchor, RecordKind::Artifact, 590, "clock_anchor") {
    DomainId      domain;
    Ticks         local_ticks;     // domain clock (TSC / GPU globaltimer)
    std::uint64_t wall_ns;         // CLOCK_REALTIME at the same moment
    std::uint64_t mono_ns;         // CLOCK_MONOTONIC_RAW at the same moment
};

}  // namespace pcomm::core::records
