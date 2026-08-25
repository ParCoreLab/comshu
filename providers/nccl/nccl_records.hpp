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
PCOMM_RECORD(NcclComm, RecordKind::Entity, 900, "nccl_comm") {
    Handle        comm;       // interned ncclComm_t
    std::uint64_t comm_hash;  // FNV-1a of the ncclUniqueId — equal across the comm
    std::int32_t  rank;       // this process's rank WITHIN this communicator
    std::int32_t  nranks;
    std::int32_t  cuda_dev;
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

}  // namespace pcomm::providers::nccl::records
