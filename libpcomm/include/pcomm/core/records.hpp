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
    Handle        pc;      // interned program counter
    Handle        object;  // interned mem-object id (0 if unattributed)
    std::uint64_t address;
    std::uint32_t size;
    MemOp         op;
    MemSpace      space;
    DeviceId      run_device;  // where the SM ran
    DeviceId      mem_device;  // where the memory lives
    std::uint32_t thread;
};

// ARTIFACT — one PC -> source location row (timeless, computed once).
PCOMM_RECORD(SourceLine, RecordKind::Artifact, 740, "source_line") {
    Handle        pc;
    Handle        file;  // interned path
    std::uint32_t line;
};

// EDGE — generic attribution row for non-inline relations.
PCOMM_RECORD(Attribution, RecordKind::Edge, 760, "attribution") {
    Handle        subject;
    Handle        object;
    EdgePredicate pred;
};

// ARTIFACT — one interned numeric value (a PC, a pointer). The dictionary that
// gives every Handle in every record its meaning.
//
// These used to be `intern-<domain>.tsv` sidecars: bare text, no magic, no
// version, no schema, and a reader that answered "" for a handle it could not
// find. A truncated or mismatched dictionary produced empty cells rather than an
// error, which is the same defect the stream header fixed one layer up — except
// here it silently voids every record in the capture, since a record's Handles
// are meaningless without it.
//
// Being a record rather than a sidecar is not a workaround; it is what these
// always were. Artifact means "computed once, timeless", which is exactly an
// intern entry, and it inherits the stream header, the schema entry, the
// byte-level conformance check and the versioning rules for free.
PCOMM_RECORD(InternKey, RecordKind::Artifact, 510, "intern_key") {
    std::uint64_t key;     // the raw value: pc, pointer
    Handle        handle;  // what records carry instead
    std::uint8_t  domain;  // rec::InternDomain
};

// ARTIFACT — one interned string, as (offset, length) into the capture's blob.
//
// Strings are the one thing that cannot live in a record directly: a record is a
// fixed-stride element of a packed array, which is what makes the writer a
// memcpy and lets a reader map a stream as an array. So the *index* is a record
// and the bytes live in `intern-blob.bin`, which carries its own header (see
// BlobHeader in the schema) so it cannot be silently paired with the wrong
// capture either.
PCOMM_RECORD(InternString, RecordKind::Artifact, 512, "intern_string") {
    std::uint64_t offset;  // byte offset into intern-blob.bin
    std::uint32_t length;  // bytes, not NUL-terminated
    Handle        handle;
    std::uint8_t  domain;  // rec::InternDomain
};

// ENTITY — a memory object with a lifetime [alloc, free).
//
// Id 520, in the INTERNAL range, and that placement is the point. It sat at 720
// — inside gpumem_lo=700..gpumem_hi=799 — which made "any provider may emit a
// MemObject" false by convention while true by declaration, and left gpumem as
// its only producer. gpumem obtains the owning device through NVBit, so
// answering "which GPU owns this buffer" cost instrumentation of every global
// memory instruction for a fact one driver call already knows.
//
// 520 opens an entity block in a range whose three occupants (510, 512, 590)
// were all artifacts, and leaves 522+ adjacent for EntityClose when the
// entity-lifetimes plan lands. Those two are joined at the hip — range
// containment is only sound within a lifetime — and the id map should read that
// way.
PCOMM_RECORD(MemObject, RecordKind::Entity, 520, "mem_object") {
    // SHARED across providers as of 2026-08-07: every emitter interns the
    // buffer's BASE ADDRESS in InternDomain::Pointer, so two providers that see
    // the same buffer mint the same id and a GpuAccess can resolve to a region
    // ibverbs registered.
    //
    // It was provider-local until then — gpumem minted a dense counter in
    // AllocRegistry::add (`m_allocs.size() + 1`) that never touched the intern
    // table, and since intern handles are dense 1-based uint32 too, gpumem's
    // object 3 and an interned pointer handle 3 were the same number naming
    // different buffers. Joining on it would have fabricated a device
    // disagreement out of a collision.
    //
    // pcomm-read still merges by `range`, not by this id. That is deliberate:
    // the range is the fact every emitter agrees on WITHOUT having to agree on a
    // convention, so it keeps working if a fourth emitter arrives interning
    // something else. The id being joinable is what a query gains; it is not
    // what the merge depends on.
    Handle   id;
    Handle   name;  // interned symbol/type name
    MemRange range;
    DeviceId device;
};

// EVENT — an entity's lifetime ended: dereg_mr, destroy_qp, devx_umem_dereg,
// devx_free_uar. One shared type rather than a close record per entity, because
// `subject` is an interned Handle and those are unique across providers, so no
// provider needs a new id to say "this one is gone".
//
// Every entity pcomm records has a lifetime and, until now, none recorded its
// end: every destroy path was hooked and none emitted anything. So a capture
// could answer "how many registration calls happened" but not "how many regions
// were live at time T" — which is why case 06's region count tracks NVSHMEM's
// allocator churn and had to retreat to per-rank symmetry, and why range
// containment in pcomm-read is only sound as long as no address is reused.
//
// A separate close record rather than back-filling the open record's Temporal
// window: the writer is an fwrite of a packed array with no index, so revising
// an already-written record would mean buffering every open entity until its
// close — unbounded — and reordering the stream. That is a different writer, not
// a change to this one.
//
// A close naming a subject no open record created is NOT a bug in the close
// hook. It is the same blindness as case 08's unaccounted queue pair arriving
// from the other side: evidence of a creation door we do not hook. Count and
// report those; suppressing them discards the only signal that would find the
// next missing door.
PCOMM_RECORD(EntityClose, RecordKind::Event, 522, "entity_close") {
    Handle subject;  // the interned handle the open record carries
    // The type_id of the record that SHOULD have opened this subject —
    // IbMemRegion 100, IbQueuePair 102, Mlx5Umem 200, Mlx5Uar 202. A record id
    // rather than a new enum, so no vocabulary has to be invented or kept in
    // sync, and a provider added later needs nothing here.
    //
    // Without it an unmatched close says only "something closed that nothing
    // opened", which is a signal you cannot act on: hooking ibv_reg_dmabuf_mr
    // closed NVSHMEM's half of that gap (case 07) and left NCCL's untouched
    // (case 02, ~42 unmatched per rank before and after), and the capture could
    // not say whether the residue was regions or queue pairs. With this it can.
    std::uint16_t opener;
};

// ARTIFACT — clock-domain calibration anchor (UNIFICATION §4). Emitted at
// start/end per domain so pcomm-read can fit offset + skew.
// ENTITY — which machine this rank ran on.
//
// Exists because a device ordinal is NODE-LOCAL and therefore does not name a
// GPU on its own: rank 0 and rank 4 of an 8-rank two-node run both report
// `run_device 0` and they are different hardware. Turning a `gpu_access`
// peer-device into a rank needs `(node, device) -> rank`, and before this record
// nothing in a capture said which node a rank was on.
//
// The reader can *infer* it from LIDs — ranks sharing an HCA port share a node —
// and does when this record is absent, so older captures still resolve. But that
// inference needs InfiniBand, and fails on exactly the runs that need it most: a
// pure-NVLink capture has no LID at all, so every NVLink edge in nccl_p2p and
// p2p_kernel resolved to "unknown peer". This record is what closes that.
//
// A hash rather than the hostname: the id only has to be *comparable between
// ranks of one job*, and putting a machine name on disk in every capture is a
// cost with no matching benefit. Collisions across a job would merge two nodes,
// which 64 bits of FNV-1a makes not worth defending against.
//
// Emitted once per rank by the engine at start, not by a provider: every
// provider needs it and none owns it — the same reasoning that put MemObject in
// the internal range rather than gpumem's.
PCOMM_RECORD(NodeIdentity, RecordKind::Entity, 524, "node_identity") {
    std::uint64_t node_hash;  // FNV-1a of the hostname; 0 if it could not be read
};

// EVENT — one kernel's EXECUTION, [start, end] on the device.
//
// Id 530, in the internal range beside MemObject/EntityClose/NodeIdentity, and
// that placement is the argument. `id_range::cupti_lo = 300` was reserved for
// this and it is the wrong home: it names an INSTRUMENT, and this record has
// two of them. Measured (tests/hpc/probes/kerneltime, MN5 job 44546506): NVBit
// dlopens libcupti and takes the process's single CUPTI subscriber slot, so
// CUPTI capture and gpumem are mutually exclusive. A capture must still know
// when kernels ran under either, which makes this the engine's record with
// providers feeding it — the same conclusion object tracking reached in
// PLAN-20260810-0505, for the same reason: "does this capture know X" must not
// answer "only if you ran NVBit".
//
// **The interval is EXECUTION, never submission.** They are not near each
// other: in the async regime every real workload uses, the launch API call
// precedes execution by 9-13 ms while the kernels themselves last 11-20 us —
// roughly 800x the kernel's own duration. A submission-timed row is legal here
// only with `source = HostSubmit`, which exists so that row can be excluded
// rather than silently drawn as a lifetime.
//
// Field order is largest-first so the struct packs to 48 B with one tail hole,
// matching the FlatBuffers struct byte for byte (schema_conformance checks it).
PCOMM_RECORD(GpuKernel, RecordKind::Event, 530, "gpu_kernel") {
    // Joins a submission to its execution when both are recorded. CUPTI's
    // correlationId natively; a launch-site counter for the other sources. 0
    // when the source cannot supply one.
    std::uint64_t correlation;
    std::uint64_t stream;  // CUstream — the lane within a device
    Handle        name;    // interned demangled symbol
    std::uint32_t grid_x;
    std::uint32_t grid_y;
    std::uint32_t grid_z;
    DeviceId      device;
    std::uint16_t block_x;
    std::uint16_t block_y;
    std::uint16_t block_z;
    std::uint8_t  source;  // KernelSource — which instrument produced [a, b]
};

PCOMM_RECORD(ClockAnchor, RecordKind::Artifact, 590, "clock_anchor") {
    DomainId      domain;
    Ticks         local_ticks;  // domain clock (TSC / GPU globaltimer)
    std::uint64_t wall_ns;      // CLOCK_REALTIME at the same moment
    std::uint64_t mono_ns;      // CLOCK_MONOTONIC_RAW at the same moment
};

}  // namespace pcomm::core::records
