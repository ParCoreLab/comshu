#pragma once
// L0 — concrete record types for phase 1 (gpumem 700-range + shared kinds).
// One example of each archetype exists from day one so the writer/reader
// handle all four paths. Network records (100s) are ported with the ibverbs
// provider; more gpumem records land with the Snoopie port.

#include <cstdint>

#include "comshu/core/ids.hpp"
#include "comshu/core/record.hpp"
#include "comshu/core/vocab.hpp"

namespace comshu::core::records {

// EVENT — one GPU memory access (the firehose). snoopie's per-access row,
// normalized: high-cardinality fields are interned Handles; the object link
// is inline (hot 1:1 edge), richer relations become Edge rows.
COMSHU_RECORD(GpuAccess, RecordKind::Event, 700, "gpu_access") {
    Handle        pc;      // interned program counter
    Handle        object;  // interned mem-object id (0 if unattributed)
    std::uint64_t address;
    // Which kernel launch issued it — GpuKernel.correlation, same rank. 0 when
    // unknown (a launch path we do not intercept, or kernel timing off).
    //
    // THIS IS A JOIN KEY AND NOT A TIME, and the distinction is why the field
    // exists. The temporal on this record is a Window of [launch enqueue, host
    // drain] in host TSC — sound, but measured at 1.94 s wide against a 48 us
    // kernel on hubspoke-44713195, because "drain" is in practice the
    // end-of-run flush. The kernel's own %globaltimer span is three orders
    // tighter, and it CANNOT be resolved when this record is written: t1 is a
    // max over exits that closes only when the last block retires, and t0 a min
    // over entries, so reading either at drain time could yield a window that
    // EXCLUDES the true access time — worse than a wide one, per
    // plans/PLAN-20260803-1021.
    //
    // So the producer records the key and the reader does the join, where the
    // whole capture is available and the ordering problem does not exist. See
    // _tl_raw in comshu-read/link_sql.hpp, which narrows to the span's window
    // when one matches and keeps the TSC window when none does.
    std::uint64_t launch;
    std::uint32_t size;
    MemOp         op;
    MemSpace      space;
    DeviceId      run_device;  // where the SM ran
    DeviceId      mem_device;  // where the memory lives
    std::uint32_t thread;
};

// ARTIFACT — one PC -> source location row (timeless, computed once).
COMSHU_RECORD(SourceLine, RecordKind::Artifact, 740, "source_line") {
    Handle        pc;
    Handle        file;  // interned path
    std::uint32_t line;
};

// EDGE — generic attribution row for non-inline relations.
COMSHU_RECORD(Attribution, RecordKind::Edge, 760, "attribution") {
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
COMSHU_RECORD(InternKey, RecordKind::Artifact, 510, "intern_key") {
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
COMSHU_RECORD(InternString, RecordKind::Artifact, 512, "intern_string") {
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
COMSHU_RECORD(MemObject, RecordKind::Entity, 520, "mem_object") {
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
    // comshu-read still merges by `range`, not by this id. That is deliberate:
    // the range is the fact every emitter agrees on WITHOUT having to agree on a
    // convention, so it keeps working if a fourth emitter arrives interning
    // something else. The id being joinable is what a query gains; it is not
    // what the merge depends on.
    Handle   id;
    Handle   name;  // interned symbol/type name
    MemRange range;
    DeviceId device;
    // Why `device` is what it is — above all, whether a kUnknownDevice means
    // "host memory" or "we never asked". See AttrStatus in vocab.hpp. Sits in
    // the record's tail padding, so the stride does not change.
    AttrStatus attr_status;
};

// EVENT — an entity's lifetime ended: dereg_mr, destroy_qp, devx_umem_dereg,
// devx_free_uar. One shared type rather than a close record per entity, because
// `subject` is an interned Handle and those are unique across providers, so no
// provider needs a new id to say "this one is gone".
//
// Every entity comshu records has a lifetime and, until now, none recorded its
// end: every destroy path was hooked and none emitted anything. So a capture
// could answer "how many registration calls happened" but not "how many regions
// were live at time T" — which is why case 06's region count tracks NVSHMEM's
// allocator churn and had to retreat to per-rank symmetry, and why range
// containment in comshu-read is only sound as long as no address is reused.
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
COMSHU_RECORD(EntityClose, RecordKind::Event, 522, "entity_close") {
    Handle subject;  // the interned handle the open record carries
    // The type_id of the record that SHOULD have opened this subject. A record
    // id rather than a new enum, so no vocabulary has to be invented or kept in
    // sync, and a provider added later needs nothing here.
    //
    // DELIBERATELY NOT ENUMERATED. This comment used to list four openers and
    // was stale by twelve: sixteen record types are closed today, across
    // ibverbs, mlx5dv, ucx, nccl and the engine. The list going stale is the
    // mechanism working — "a provider added later needs nothing here" is the
    // whole point, and a comment that must be edited every time one is added is
    // a comment that will not be. For the current set:
    //
    //     grep -rn 'EntityClose\|RecordTrait<.*>::type_id' providers/ libcomshu/src/
    //
    // Without it an unmatched close says only "something closed that nothing
    // opened", which is a signal you cannot act on: hooking ibv_reg_dmabuf_mr
    // closed NVSHMEM's half of that gap (case 07) and left NCCL's untouched
    // (case 02, ~42 unmatched per rank before and after), and the capture could
    // not say whether the residue was regions or queue pairs. With this it can.
    std::uint16_t opener;
};

// ARTIFACT — clock-domain calibration anchor (UNIFICATION §4). Emitted at
// start/end per domain so comshu-read can fit offset + skew.
// ARTIFACT — which machine this rank ran on.
//
// Artifact and not Entity, as of 2026-08-25. It has no lifetime: it does not
// open, nothing ever closes it, and it is emitted once per rank at start. The
// Entity definition is "a thing with a lifetime — it opens and it closes", and
// applying that to a per-rank constant made the archetype mean two things at
// once. See libcomshu/schema/README.md, "The archetypes are a test, not a
// label".
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
COMSHU_RECORD(NodeIdentity, RecordKind::Artifact, 524, "node_identity") {
    std::uint64_t node_hash;  // FNV-1a of the hostname; 0 if it could not be read
};

// ARTIFACT — what a device ORDINAL actually refers to, on this rank.
//
// THE PROBLEM. Every other record in this project names a GPU by its runtime
// ordinal, and an ordinal is not an identity in two independent ways. It is
// node-local, which comshu.fbs has always said. It is also REMAP-local, which
// nothing said until now: CUDA_VISIBLE_DEVICES renumbers per process, so four
// ranks each pinned with CUDA_VISIBLE_DEVICES=$LOCAL_RANK all report device 0
// and _device_owner hands every NVLink peer on that node to min(rank).
//
// MEASURED, NOT ARGUED — job 45280114, four MN5 nodes, sixteen H100s:
//
//   node A   0 -> 00000000:1B:00.0    node B   0 -> 00000000:1B:00.0
//            1 -> 00000000:2C:00.0             1 -> 00000000:2C:00.0
//            2 -> 00000000:9D:00.0             2 -> 00000000:9D:00.0
//            3 -> 00000000:AD:00.0             3 -> 00000000:AD:00.0
//
// Identical hardware, so `pci_bus` REPEATS EXACTLY across nodes and is not a
// global identity either. `(node_hash, pci_bus)` is, and pci_bus alone is
// actively wrong — that is why NodeIdentity is a hard dependency of this record
// rather than a nicety.
//
// `uuid_hi`/`uuid_lo` are the 16 raw bytes of cuDeviceGetUuid, and they are a
// CROSS-CHECK, not the key. On the two machines measured they are healthy
// (sixteen distinct random values, no zeros, nothing sequential), but a UUID
// comes back zero-filled or as small incrementing integers under vGPU, some
// container runtimes and older drivers — and a degenerate UUID is WORSE than
// none, because "GPU-0" on two nodes looks identical and merges them while
// looking authoritative. Keeping it off the key means a bad one cannot corrupt
// ownership; it can only fail to confirm it.
//
// NOT MIG-AWARE, deliberately and on the record. A MIG instance reports
// MIG-GPU-<uuid>/<gi>/<ci> and shares its parent UUID with every sibling, so a
// key built on the parent would merge devices that are genuinely separate. MIG
// is Disabled on all sixteen GPUs measured, so that path could be written but
// not tested, and an untested path in an identity record is how a confidently
// wrong owner gets shipped. Scoped out 2026-09-01 by decision, not oversight.
//
// One row per VISIBLE device, not per used device. This is a catalogue —
// "what is device 2 on this node, really" — and is deliberately NOT what
// rank_identity.device reads: that answers "which GPU did this rank drive",
// derives from evidence of USE, and would report devices = 4 for every rank on
// a node if it read visibility instead.
//
// Emitted by the engine rather than a provider, for NodeIdentity's reason: every
// provider needs it and none owns it.
COMSHU_RECORD(GpuDevice, RecordKind::Artifact, 528, "gpu_device") {
    std::uint64_t uuid_hi;   // cuDeviceGetUuid bytes 0..7   — cross-check only
    std::uint64_t uuid_lo;   // cuDeviceGetUuid bytes 8..15  — 0,0 if unavailable
    std::uint32_t pci_bus;   // domain<<16 | bus<<8 | device; function is always 0
    Handle        name;      // interned, e.g. "NVIDIA H100"
    DeviceId      ordinal;   // the runtime ordinal every other record uses
};

// ARTIFACT — what this rank was ASKED to do, and by which build.
//
// EVERY OTHER RECORD IN THIS FILE SAYS WHAT THE APPLICATION DID. None said what
// the TRACER was told to do, and a capture that cannot describe its own inputs
// is not self-contained. Jobs 45329318 and 45330017 differ ONLY in mpirun's
// `--map-by`, produce measurably different traffic, and nothing inside either
// capture says which is which. The resulting placement is recoverable from
// node_identity plus rank — comm_edges.scope proves it — but the REQUEST that
// produced it was recorded nowhere, so the two directories are distinguishable
// only by the shell history of whoever launched them.
//
// `images` IS THE FIELD WITH A SCAR. Job 45331793 reported
// uct_binding.accepted = 0 on a UCX version the guard had just been widened to
// admit, which read as a clean refutation of the change. It was not. That job
// loads `.so` files hand-copied into a second directory, and the copy was two
// hours stale while the build tree beside it was correct — there were SIX UCX
// build directories on that machine and one job pointing at copies from one of
// them. I nearly reported a stale binary as evidence against my own change.
//
// So this walks /proc/self/maps at emit time and records every mapped libcomshu
// object with its size and mtime. No cooperation from providers, no ABI change,
// and it answers "which file actually answered" for the engine and every
// provider at once — which is strictly more than a commit baked into the engine
// could say about a provider copied out of a different tree.
//
// `build` is the compile-time counterpart: commit, dirty flag, build type,
// engine ABI, schema hash. Regenerated on EVERY build rather than at configure
// time, because a build id that only refreshes when someone re-runs cmake is
// precisely the stale-provenance failure this record exists to catch.
//
// `env` IS AN ALLOWLIST, NOT THE ENVIRONMENT. Prefix COMSHU_, plus the launcher
// variables that actually decide placement — SLURM_*, OMPI_MCA_rmaps_*,
// CUDA_VISIBLE_DEVICES, LD_PRELOAD. COMSHU_ENV_CAPTURE extends the prefix list.
// Writing all of environ would put credentials and home paths into every
// capture, which is a real cost for the two variables anyone wants.
//
// EMITTED LAZILY, NOT AT START. Every payload field is an interned string, and
// the dictionary those handles resolve against is only written at teardown — so
// unlike NodeIdentity and ClockAnchor this record cannot join the early flush
// that defends against a signal. See Tracer::start for why those two can and
// this one cannot.
//
// TWO SITES, and the second is a backstop the first needs. The arena flush is
// preferred, because it runs while the process is alive and `providers` is
// complete by then. But it only fires when a record buffer FILLS, so a short
// capture never reached it — measured on MN5 job 45344306, where ucx_deep,
// ib_pingpong and ib_send_bw carried no invocation row at all. Tracer::finalize
// therefore calls it too; nothing this record reads goes away at teardown, so
// unlike GpuDevice it can be written there.
COMSHU_RECORD(Invocation, RecordKind::Artifact, 532, "invocation") {
    Handle        argv;       // /proc/self/cmdline, NULs rendered as spaces
    Handle        env;        // captured variables, one "K=V" per line
    Handle        providers;  // every provider that declared itself, comma-joined
    Handle        build;      // commit, dirty, build type, ABI, schema hash
    Handle        images;     // mapped libcomshu objects: "path size mtime" per line
    std::uint32_t pid;        // this rank's pid, which the rank number may also be
};

// ENTITY — one process's mapping of a shared allocation, keyed by an identity
// both sides of the share agree on.
//
// THE PROBLEM THIS EXISTS FOR. A peer access arrives at an address in the
// ACTOR's mapping of someone else's memory. To place it in an object we need the
// OWNER's address for the same byte, and CUDA's VMM API will map a peer's
// allocation while telling you nothing about which allocation it was. NVSHMEM
// escapes this because nvshmem_ptr(addr, pe) IS that correspondence; nothing
// below the library offers an equivalent, which is why object_access drops
// non-symmetric peer accesses today (link_health.unattributed_p2p_accesses).
//
// `allocation` is the join key and the whole point: it is the interned
// CUmemFabricHandle, hex-encoded, which is globally meaningful by construction —
// that is what a fabric handle is FOR. Two ranks that share memory mint the same
// id from the same 64 bytes, so the exporter's row and the importer's row meet.
//
// WHY ONLY FABRIC. The POSIX-fd handle type cannot do this and it was measured,
// not assumed: on MN5 (job 44705498) all 48 exports and imports fstat'd to the
// same driver-node inode, dev=6 ino=1729, so an fd names the driver rather than
// the allocation and its NUMBER is process-local anyway. A row is therefore
// written only for CU_MEM_HANDLE_TYPE_FABRIC, and a capture with none says so by
// being empty rather than by carrying a key that does not join.
//
// `alloc_offset` is cuMemMap's own `offset` argument: where this mapping starts
// inside the allocation. Both sides may map different parts, so the address
// arithmetic is (my_base + d) -> allocation offset (my_alloc_offset + d) ->
// (their_base + that - their_alloc_offset). Carrying it is what makes partial
// maps work rather than assuming everyone maps the whole thing at zero.
COMSHU_RECORD(PeerMapping, RecordKind::Entity, 526, "peer_mapping") {
    Handle        allocation;    // interned fabric handle (hex) — the global key
    MemRange      range;         // this process's mapping: [base, size)
    std::uint64_t alloc_offset;  // where `range.base` sits inside the allocation
    DeviceId      device;        // which GPU the memory lives on
    std::uint8_t  exporter;      // 1 = we created and exported it; 0 = imported
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
COMSHU_RECORD(GpuKernel, RecordKind::Event, 530, "gpu_kernel") {
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

// EVENT — one host-initiated memory COPY, [start, end] on the copy engine.
//
// THE HOLE THIS FILLS, and it is not a bookkeeping one. `comshu/run.cpp`'s
// registry says of gpumem: "It does not capture DMA: NVBit rewrites SASS, so a
// copy-engine cudaMemcpy executes no instruction and is invisible here." That
// is true of every provider in this tree, not only of gpumem — a copy engine
// retires no instruction on any SM, posts no work request on any queue pair,
// and calls into neither UCX nor NCCL. So until this record a cudaMemcpy
// appeared in NO table at all.
//
// And a peer copy IS COMMUNICATION. cudaMemcpyPeer moves bytes between two
// GPUs over NVLink or PCIe; it is what a hand-rolled halo exchange or a
// model-parallel weight shuffle is built out of, and it resolves to a rank->rank
// edge exactly as an ib_post_send does. A workload whose entire inter-GPU
// traffic is cudaMemcpyPeerAsync used to produce an EMPTY comm_edges and a
// clean link_health.
//
// Id 534, in the internal range beside GpuKernel (530), and NOT in
// id_range::cupti_lo = 300 which is still reserved and still unused. GpuKernel
// declined that range because "it names an INSTRUMENT, and this record has two
// of them"; this record has one instrument today, so the letter of that
// argument does not apply and its substance does: "does this capture know X"
// must not answer "only if you ran CUPTI". A second instrument for this fact is
// already named — interposing cudaMemcpy*/cuMemcpy* needs no CUPTI subscriber
// and is specifically what would work under gpumem, where CUPTI is refused —
// and numbering the record in the CUPTI range would force that follow-up either
// to emit from a range whose name is a lie or to mint a second family for one
// fact. See plans/PLAN-20260903-1828.
//
// **The interval is EXECUTION, from the copy engine.** `source` is what says
// so, for KernelSource's reason: a HostApi row would be the API call, which is
// a real bound for a synchronous copy and submission — milliseconds early — for
// an async one. Two different claims, and a consumer has to be able to tell.
//
// WHAT THIS RECORD STRUCTURALLY CANNOT CARRY, stated here because the absence
// is a design fact and not an oversight: **addresses**. Neither
// CUpti_ActivityMemcpy6 nor CUpti_ActivityMemcpyPtoP4 has a pointer-valued
// field of any kind — the peer record's identity fields are srcDeviceId,
// srcContextId, dstDeviceId, dstContextId and nothing else. So a copy cannot be
// placed inside a mem_object at either end, and object_access does not see this
// traffic. That is the concrete thing an API-interposition source would unlock,
// since the pointers are its arguments.
COMSHU_RECORD(CudaMemcpy, RecordKind::Event, 534, "cuda_memcpy") {
    // CUPTI's correlationId, and it is the SAME id space GpuKernel.correlation
    // and GpuAccess.launch live in — one join key across three records rather
    // than three private ones. 0 when the source cannot supply one.
    std::uint64_t correlation;
    std::uint64_t bytes;
    // The stream ORDINAL, not a CUstream, matching GpuKernel.stream: CUPTI
    // reports the ordinal and a timeline lane wants the ordinal anyway.
    std::uint64_t stream;
    // kUnknownDevice — NOT 0 — when that end of the copy is host memory. 0 is a
    // real GPU, and a plausible value standing in for a missing one is the
    // failure family this tree keeps hitting; see core/vocab.hpp.
    //
    // For a NON-peer copy both ends that are on a device carry the same
    // ordinal, because CUPTI's non-peer record has exactly one deviceId. That
    // is what makes `src_device <> dst_device` a sound edge test rather than an
    // optimistic one: a row can only claim two devices when CUPTI reported two.
    DeviceId src_device;
    DeviceId dst_device;
    // Which device's CONTEXT the copy was issued in — CUPTI's own `deviceId`,
    // which it reports on the peer record too, beside srcDeviceId/dstDeviceId.
    //
    // THIS IS THE OWNERSHIP FIELD, and it is the difference between a copy
    // matrix in rank space and one that resolves to nothing. Measured on du04:
    // four MPI ranks, one GPU each, cudaMemcpyPeer'ing round a ring through CUDA
    // IPC. The 400 records were perfect in DEVICE space — rank 0 says 0->1,
    // rank 1 says 1->2, and so on — and comm_edges collapsed the whole ring to a
    // single `-1 -> -1` edge with unresolved_device_copy = 400, because NOTHING
    // in that capture said which rank drove which GPU. The evidence
    // _device_owner accepts is mem_object (cudaMalloc is deliberately not
    // hooked), gpu_access.run_device (needs gpumem, which cannot share a process
    // with CUPTI), gpu_kernel.device (that workload launches no kernels) and
    // nccl_comm (no NCCL). A pure-peer-copy program supplies none of them.
    //
    // NEITHER src_device NOR dst_device CAN SUBSTITUTE, which is why this is a
    // field rather than an inference. A peer copy names one device the rank owns
    // and one it does not, and the record does not say which is which:
    // cudaMemcpyPeer(dst, dstDev, src, srcDev, n) is equally legal pulling from
    // a peer as pushing to one. Guessing would put another rank's traffic under
    // the wrong owner — the failure _device_owner's own comment refuses buffer
    // residency for.
    //
    // A context, by contrast, belongs to the process that made it current. So
    // this is admitted as strong ownership evidence for every copy row, while
    // src_device/dst_device are admitted only to _device_key, which translates
    // ordinals and decides nothing.
    DeviceId     ctx_device;
    MemcpyKind   kind;       // what the API was asked for
    MemResidency src_space;  // what kind of allocation the bytes came from
    MemResidency dst_space;  // ... and landed in
    CopySource   source;     // which instrument produced [a, b]
    // 1 when the copy was issued through an asynchronous entry point. A fact
    // about the API, not about the engine: an async copy and a sync one look
    // identical from the copy engine, and the difference is whether the calling
    // thread was blocked — which is exactly the thing a timeline reader wants
    // and cannot otherwise recover.
    std::uint8_t async;
};

COMSHU_RECORD(ClockAnchor, RecordKind::Artifact, 590, "clock_anchor") {
    DomainId      domain;
    Ticks         local_ticks;  // domain clock (TSC / GPU globaltimer)
    std::uint64_t wall_ns;      // CLOCK_REALTIME at the same moment
    std::uint64_t mono_ns;      // CLOCK_MONOTONIC_RAW at the same moment
};

// ARTIFACT — one row per rank saying whether that rank's capture is COMPLETE.
// Id 592, beside ClockAnchor and for its reason: both are facts about the run
// rather than about anything the application did.
//
// WHY THIS RECORD EXISTS, and it is not the reason the loss counters exist.
// FileSink has checked every write since 2026-09-03 and Tracer::report_losses
// prints `CAPTURE INCOMPLETE` when one fails. Both are true and both are
// EPHEMERAL: they live in the stderr of the traced process. What gets copied off
// a cluster and analysed weeks later is the Parquet, and that carried no trace
// of it at all. Measured on du04 — one real capture, damaged the three ways this
// sink can fail, run through `comshu export` and `comshu link`:
//
//     variant      mlx5_umem rows   ranks   link_health non-zero
//     pristine          10            2          0 of 25
//     deleted            5            1          0 of 25
//     zeroed             5            1          0 of 25
//     truncated          9            2          0 of 25
//
// Half the table gone and one of two ranks gone entirely, and the health rubric
// returned a clean bill of health four times out of four. That is the same
// failure `unresolved_nvlink` was added for — "a capture with NO node identity
// at all reported a clean bill of health while 100% of its GPU traffic was
// unattributed" (link_sql.hpp) — arriving one layer lower down.
//
// PRESENCE IS THE ATTESTATION, and it is the half that survives what the
// counters cannot. Tracer::report_losses argues, correctly, that a record
// describing "writes to this directory are failing" would be written through the
// sink that is failing. So this record does not try to be reliable. It is
// emitted ONLY from a normal finalize(), never from flush_from_signal(), which
// makes its ABSENCE meaningful in a way a loss counter can never be:
//
//     row present, all zero  -> this rank finished and lost nothing
//     row present, non-zero  -> this rank finished and lost exactly this much
//     NO ROW for a rank      -> unknown. It was killed, it faulted, or it could
//                               not write even these 32 bytes. Not "healthy".
//
// The third state is the one that was missing, and it is worth more than write
// loss alone: a rank whose process was OOM-killed mid-run has always produced a
// short capture that read as a quiet one. `clock_anchor` is written at start for
// every rank, so a rank in clock_anchor with no capture_status here is exactly
// "started but never finished" — see link_health.capture_unattested_ranks.
//
// The counts EXCLUDE this record's own stream, which is written after the
// snapshot is taken. A failure to write the attestation therefore shows up as
// the attestation being absent or short, which is the third state above and not
// a fourth thing to reason about.
//
// No Handle fields, deliberately: the intern dictionary has already been written
// by the time finalize() gets here, so a string in this record would resolve to
// nothing. Everything is a number.
COMSHU_RECORD(CaptureStatus, RecordKind::Artifact, 592, "capture_status") {
    std::uint64_t records_lost;     // rounded UP; a partial record is a lost one
    std::uint64_t bytes_lost;       // what the sink knows exactly
    std::uint32_t streams_open;     // streams framed, INCLUDING this record's own
    std::uint32_t streams_lost;     // how many streams lost anything at all
    std::uint8_t  dir_failed;       // the output directory itself could not be made
    std::uint8_t  header_lost;      // >=1 stream has no header and is unreadable
    std::uint8_t  never_opened;     // >=1 stream could not be opened at all
    std::uint8_t  pad;
    std::uint32_t pad2;
};

}  // namespace comshu::core::records
