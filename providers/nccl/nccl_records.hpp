#pragma once
// NCCL host-API records (900-999). These describe the COLLECTIVE LAYER as the
// application asked for it: which communicator, which operation, over which
// buffers, with how many elements of what type.
//
// WHY THIS LAYER AND NOT THE ACCESSES. Case 05 records 1,179,948 GPU accesses
// under NVBit and places none of them in a tracked object, because NCCL's LL
// protocol spins on its own internal channel buffers (ncclCudaCalloc ->
// cudaMalloc or ncclCuMemAlloc, src/include/alloc.h:452) which sit ~14 MiB above
// the user buffer and which the application never allocated. Attributing that
// flag-polling traffic to "communication" would be a fabricated number. The
// operation layer is where NCCL's communication is actually stated, and it is
// stated exactly once per call.
//
// EVERY TEMPORAL HERE IS AN ENQUEUE, NOT AN EXECUTION. NCCL collectives are
// stream-ordered: the host call returns once the work is queued, and the launch
// API call precedes execution by milliseconds while the kernel lasts
// microseconds (the same regime KernelSpan's `source = HostSubmit` exists to
// mark). The interval on these records is the duration of the HOST CALL. It is
// not the duration of the collective, nothing in this file claims it is, and a
// reader that renders it as one is reporting a number that does not exist.
// Joining to a kernel span is how you get execution.

#include <cstdint>

#include "pcomm/core/record.hpp"
#include "pcomm/core/vocab.hpp"

namespace pcomm::providers::nccl::records {
using namespace pcomm::core;

// WHICH COLLECTIVE. Values are pinned: they go on disk.
//
// Kept as one enum on one record rather than a table per collective, for the
// reason ib_post_send folds send/write/read together: a reader asking "what did
// this communicator do" should not have to union six tables to find out. The
// discriminator is never dropped, because the traffic pattern of an AllGather
// and a Reduce are not the same shape and a raw byte count cannot tell them
// apart.
enum class CollOp : std::uint8_t {
    AllReduce = 0,
    Broadcast = 1,
    Reduce = 2,
    AllGather = 3,
    ReduceScatter = 4,
    AllToAll = 5,  // ncclAllToAll where the build has it
};

// Send or receive. Point-to-point is a SEPARATE record from the collectives,
// not a CollOp value, because it is the only NCCL call that names a peer — and
// a peer is the one field from which a comm edge can be built without knowing
// the collective's internal algorithm.
enum class P2pDir : std::uint8_t {
    Send = 0,
    Recv = 1,
};

// Group boundary. ncclGroupStart/End fuse the calls between them into one
// launch, so a run of ncclSend/ncclRecv inside a group is ONE operation on the
// wire and not N. Without these rows a reader counting point-to-point calls
// overcounts launches by exactly the group size, and an alltoall built from
// 2*(n-1) grouped calls looks like 2*(n-1) separate transfers.
enum class GroupEdge : std::uint8_t {
    Start = 0,
    End = 1,
};

// The communicator. `comm_hash` is what makes this joinable ACROSS RANKS.
//
// A ncclComm_t is a process-local pointer, so it is worth nothing on its own —
// the same discipline ucx_endpoint learned from qp_num. The unique id every
// rank of a communicator is initialised with IS the shared name, so it is
// hashed here (FNV-1a over all 128 bytes, the same construction
// ucx_worker_address uses and for the same reason: deterministic everywhere, no
// process salt). Two rows on two ranks with one comm_hash are two members of
// one communicator, and that is the join the comm matrix needs.
//
// `nccl_comm_hash` is a SECOND hash of the same identity, and the two are not
// interchangeable. `comm_hash` is ours and is the cross-RANK join. This one is
// NCCL's own `comm->commHash`, recomputed by us from the ncclUniqueId (see
// nccl_comm_hash.hpp), and it is the cross-LAYER join: it is the only name the
// profiler plugin is ever told, so without it a nccl_coll_exec row cannot be
// attached to the communicator it belongs to.
//
// It is 0 where NOT DERIVABLE — communicators built by ncclCommSplit or by the
// grow path get a commHash mixed from their parent, which no ncclUniqueId we
// are shown determines. 0 means "unknown", never "no communicator", and a join
// on it must exclude 0 rather than treat it as a value.
PCOMM_RECORD(NcclComm, RecordKind::Entity, 900, "nccl_comm") {
    Handle        comm;            // interned ncclComm_t
    std::uint64_t comm_hash;       // FNV-1a of the ncclUniqueId — equal across the comm
    std::uint64_t nccl_comm_hash;  // NCCL's own commHash; 0 = not derivable
    std::int32_t  rank;            // this process's rank WITHIN this communicator
    std::int32_t  nranks;
    std::int32_t  cuda_dev;
    std::int32_t  pad;
};

// One collective call. Both buffers are interned so they join to mem_object
// when the application allocated them through a door the engine sees.
//
// `count` is elements, `datatype` is NCCL's enum, and they are kept apart
// rather than multiplied into a byte count, because the product is LOGICAL
// bytes — what the application handed over — and that is not what crosses the
// wire. A ring all-reduce moves about 2x(n-1)/n of it per rank; a broadcast
// moves it once from one rank. Presenting the logical size as traffic would
// invent a measurement, so the record carries the two facts it actually knows
// and leaves the reader to name what it derives.
PCOMM_RECORD(NcclCollective, RecordKind::Event, 902, "nccl_collective") {
    Handle        comm;
    Handle        sendbuff;
    Handle        recvbuff;
    Handle        stream;    // interned cudaStream_t — the lane this was queued on
    std::uint64_t count;     // ELEMENTS, not bytes
    std::int32_t  root;      // -1 where the op has no root
    std::uint8_t  op;        // CollOp
    std::uint8_t  datatype;  // ncclDataType_t
    std::uint8_t  redop;     // ncclRedOp_t; 0xff where the op does not reduce
    std::uint8_t  pad;
};

// One point-to-point call. The peer is the whole point: this is the only NCCL
// record from which an exact edge follows, with no assumption about how a
// collective's algorithm decomposes.
PCOMM_RECORD(NcclP2p, RecordKind::Event, 904, "nccl_p2p") {
    Handle        comm;
    Handle        buffer;
    Handle        stream;
    std::uint64_t count;  // ELEMENTS
    std::int32_t  peer;   // rank within `comm`
    std::uint8_t  dir;    // P2pDir
    std::uint8_t  datatype;
    std::uint16_t pad;
};

// Group boundaries. Cheap to record and impossible to reconstruct afterwards:
// nothing in a send row says whether it was fused with the next one.
// `depth` is the NESTING depth this call sits at, counted per thread: 1 for an
// outermost ncclGroupStart, higher inside a nested one. NCCL allows nesting and
// only the OUTERMOST ncclGroupEnd launches, so without this a reader cannot
// tell which End is the one that submits work — every inner End looks like a
// launch boundary and an alltoall built from nested groups reads as several.
//
// `edge` is widened to 32 bits rather than padded to the record's alignment.
// Seven bytes of declared padding would be seven bytes this file has to keep
// truthful across two schemas forever, in exchange for nothing.
PCOMM_RECORD(NcclGroup, RecordKind::Event, 906, "nccl_group") {
    std::uint32_t depth;
    std::uint32_t edge;  // GroupEdge
};

// A registered memory window — the host half of the DEVICE-INITIATED path.
//
// This is the record that makes GPU-initiated NCCL attributable where case 05
// is not. Device code reaches a peer through ncclGetLsaPointer(window, offset,
// peer), and the window is registered here, on the host, through a call we can
// see. So an access NVBit records inside a registered window CAN be named,
// while NCCL's internal channel buffers cannot: the difference is that the
// application allocated and registered this one.
PCOMM_RECORD(NcclWindow, RecordKind::Entity, 908, "nccl_window") {
    Handle        window;  // interned ncclWindow_t
    Handle        comm;
    Handle        buffer;  // the registered base pointer
    std::uint64_t size;
    std::int32_t  win_flags;
    std::int32_t  pad;
};

// The device communicator handed to a kernel. Its presence in a capture is the
// fact "this workload initiates communication from the GPU", which nothing else
// in a capture states: the device API is entirely NCCL_DEVICE_INLINE, so it is
// inlined into the application's kernel and leaves no symbol to interpose.
//
// It carries the communicator and nothing else. The requirements struct passed
// to ncclDevCommCreate has fields worth having (lsaBarrierCount, ginContextCount
// — how much device-side machinery was asked for), but reading them means
// depending on a struct layout that carries its own `size`, `magic` and
// `version` fields precisely because NVIDIA expects it to change. A record that
// silently misreads an int after an NCCL upgrade is worse than one that does
// not have it.
// Artifact, not Entity (2026-08-25): an existence fact, which is the Artifact
// definition almost word for word. Nothing opens or closes it.
PCOMM_RECORD(NcclDevComm, RecordKind::Artifact, 910, "nccl_dev_comm") {
    Handle       comm;
    std::int32_t pad;
};

// ---------------------------------------------------------------------------
// THE PROFILER-PLUGIN LAYER (912-916) — what NCCL DECIDED, not what was asked.
//
// Everything above this line is the host API: the call the application made.
// Everything below comes from NCCL's profiler plugin callbacks, which fire from
// inside the enqueue and proxy machinery and report the decisions taken there —
// the algorithm, the protocol, the channel count, and the decomposition into
// per-peer proxy operations.
//
// WHY THIS IS A SEPARATE SET OF RECORDS AND NOT FIELDS ON nccl_collective. The
// host-API records are written by an interposer that returns before NCCL has
// chosen anything; these are written by a different mechanism (a plugin NCCL
// dlopens and calls back into) at a different time, and on a run where the
// plugin is not wired up they are simply absent while the host-API rows are
// complete. Folding them together would produce a table whose columns are
// sometimes populated and sometimes not, with nothing in the row to say which —
// exactly the ambiguity that makes a capture unreadable.
//
// THE CAUSAL CHAIN IS NCCL'S, NOT OURS. Every profiler event descriptor carries
// a `parentObj`, and that pointer is the handle WE returned from startEvent for
// the parent event. So the chain coll -> proxyOp is stated by NCCL rather than
// inferred by us from timing or size, which is the whole reason this door is
// worth opening: no other surface can say which collective produced which
// per-peer traffic without guessing.
//
// WHAT THESE CANNOT SEE, so no reader assumes otherwise:
//   - Device-initiated traffic (GIN/IBGDA puts from inside a kernel) never
//     reaches the CPU proxy, so no nccl_proxy_op describes it. The algo/proto
//     on the collective row is still true; the decomposition is not there.
//   - NVLink/P2P-direct movement, for the same reason.
//   - Bytes actually delivered. chunk_size x n_steps is what NCCL PLANNED. It
//     is not a completion count and must never be reported as one.

// One communicator, as the profiler plugin was told about it. Emitted from the
// plugin's `init`, which NCCL calls once per communicator.
//
// This is the bridge row. `nccl_comm_hash` is the value NCCL passed us, and it
// joins to the identically-named column on nccl_comm — the only path from a
// profiler event back to a communicator, a rank layout, and a device.
//
// `comm_name` is the user-assigned name from ncclConfig_t, empty where unset.
// It is interned rather than dropped because it is the only human-readable name
// a communicator ever has, and a workload that names its comms ("dp", "tp",
// "pp") is telling us its parallelism strategy for free.
PCOMM_RECORD(NcclProfComm, RecordKind::Artifact, 912, "nccl_prof_comm") {
    Handle        comm_name;
    std::uint64_t nccl_comm_hash;
    std::int32_t  nnodes;
    std::int32_t  nranks;
    std::int32_t  rank;
    std::int32_t  pad;
};

// One collective, as NCCL EXECUTED it.
//
// THE TEMPORAL HERE IS NOT THE ONE ON nccl_collective, and conflating them
// produces a number that does not exist. nccl_collective's interval is the
// duration of the HOST CALL — an enqueue, milliseconds ahead of the work. This
// interval runs from NCCL's startEvent to its stopEvent for the collective
// itself, which is the operation's own lifetime inside NCCL.
//
// `algo` and `proto` are NCCL's strings ("Ring", "Tree", "NVLS", "CollnetChain";
// "Simple", "LL", "LL128"), interned as given. They are NOT normalised into an
// enum: the set is NCCL's to extend, and a build that returns a name we have not
// seen must record that name rather than fall into an "Other" bucket that
// destroys the one fact the row exists to carry.
//
// `event_id` is a per-process counter we assign; `parent_event_id` is the id of
// the event NCCL named as this one's parent, or 0 where that parent's event
// family was masked off and we therefore never saw it. 0 means "not observed",
// never "root".
PCOMM_RECORD(NcclCollExec, RecordKind::Event, 914, "nccl_coll_exec") {
    Handle        func;      // "AllReduce", "Broadcast", ... as NCCL spells it
    Handle        algo;      // "Ring" / "Tree" / "NVLS" / ...
    Handle        proto;     // "Simple" / "LL" / "LL128"
    Handle        datatype;  // NCCL's own name for it
    std::uint64_t nccl_comm_hash;
    std::uint64_t event_id;
    std::uint64_t parent_event_id;  // 0 = not observed
    std::uint64_t seq_number;       // NCCL's per-comm collective sequence number
    std::uint64_t count;  // ELEMENTS, as on nccl_collective
    // NCCL's own value, and it is NOT nccl_collective's convention. That record
    // writes -1 for an operation with no root, deliberately, because 0 is a
    // valid rank. NCCL reports 0 — measured on du04, every AllReduce,
    // AllGather and ReduceScatter row came back root=0. So a 0 here means
    // "NCCL said 0" and NOT "rank 0 is the root", and the two records must not
    // be compared on this column.
    std::int32_t root;
    std::uint8_t  n_channels;
    std::uint8_t  n_warps;
    std::uint16_t pad;
};

// One proxy operation — the row where a collective becomes per-peer traffic.
//
// This is the finest granularity at which NCCL names a PEER, and it is the
// reason this door answers "which call produced this movement" without
// inference. `parent_event_id` points at the nccl_coll_exec (or nccl_p2p event)
// that owns it; `peer` is a rank within that communicator.
//
// `pid` is NCCL's, carried on the descriptor so a plugin can tell whether the
// parent event belongs to this process — proxy threads service operations for
// other ranks on the same node. It is recorded rather than filtered on, because
// a row whose pid is not ours is a fact about shared-proxy topology, and
// dropping it would make the parent chain look broken instead of shared.
//
// n_steps x chunk_size is a PLAN. See the header comment above.
//
// `channel_id` and `is_send` are 8-bit facts widened to 32, for the reason
// NcclGroup's `edge` is: the record would otherwise need six bytes of declared
// padding, which is six bytes two schemas have to keep truthful forever in
// exchange for nothing. Widening leaves the struct exactly 8-aligned with no
// hole to keep honest.
PCOMM_RECORD(NcclProxyOp, RecordKind::Event, 916, "nccl_proxy_op") {
    std::uint64_t event_id;
    std::uint64_t parent_event_id;  // 0 = not observed
    std::int32_t  peer;             // rank within the communicator
    std::int32_t  n_steps;
    std::int32_t  chunk_size;       // bytes per step, as NCCL planned them
    std::int32_t  pid;
    std::uint32_t channel_id;
    std::uint32_t is_send;  // 1 send, 0 recv
};

}  // namespace pcomm::providers::nccl::records
