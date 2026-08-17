#pragma once
// Public-UCP records (600-699). These describe application/communication
// intent. They deliberately do not claim that raw UCP count is a byte count.

#include <cstdint>

#include "pcomm/core/record.hpp"
#include "pcomm/core/vocab.hpp"

namespace pcomm::providers::ucx::records {
using namespace pcomm::core;

enum class StatusClass : std::uint8_t {
    Immediate = 0,  // nullptr / UCS_OK
    Error = 1,      // UCS_PTR_IS_ERR
    Request = 2,    // a real request pointer
};

// WHICH ENTRY POINT a row came through. Values are pinned: they go on disk.
//
// This is not bookkeeping about UCX trivia. `ucp_tag_send_sync_nb` is what
// MPI_Ssend becomes and completes only once the receiver has matched, so a row
// from it means something different from an ordinary send that merely buffered;
// and `_nbi` returns a plain status with no request at all, so its rows are
// complete when written while an `_nbx` row may still be in flight. Folding
// these together would erase both distinctions and leave nothing in the record
// able to recover them.
//
// It also answers the coverage question directly: which doors a given machine's
// stack really drives is a per-installation fact (MN5's MPI one-sided path uses
// only `_nbi`), and with this field a capture states it rather than requiring a
// readelf sweep to guess it.
enum class UcxDoor : std::uint8_t {
    Nbx = 0,         // the modern *_nbx entry point
    Legacy = 1,      // the pre-nbx *_nb entry point
    LegacySync = 2,  // ucp_tag_send_sync_nb — completes on receiver match
    Nbi = 3,         // ucp_put_nbi / ucp_get_nbi — no request, status only
    LegacyMsg = 4,   // ucp_tag_msg_recv_nb — keyed by message handle, not tag
    LegacyPost = 5,  // ucp_atomic_post — fire and forget, no result at all
};

PCOMM_RECORD(UcxContext, RecordKind::Entity, 600, "ucx_context") {
    Handle        context;
    std::uint32_t api_major;
    std::uint32_t api_minor;
};

PCOMM_RECORD(UcxWorker, RecordKind::Entity, 602, "ucx_worker") {
    Handle worker;
    Handle context;
};

PCOMM_RECORD(UcxEndpoint, RecordKind::Entity, 604, "ucx_endpoint") {
    Handle endpoint;
    Handle worker;
};

PCOMM_RECORD(UcxTagSend, RecordKind::Event, 606, "ucx_tag_send") {
    Handle        endpoint;
    Handle        buffer;
    Handle        request;
    std::uint64_t count;  // raw UCP datatype count, not universally bytes
    std::uint64_t tag;
    std::int32_t  status;        // UCS_OK or the decoded error status
    std::uint8_t  status_class;  // StatusClass
    std::uint8_t  door;          // UcxDoor
};

// EVENT — a tagged receive was POSTED. Not a receive that completed, and the
// distinction is the whole reason this record's `count` must never be added to
// a bytes total: it is the capacity the application offered, and the message
// that eventually lands in it may be smaller, may arrive much later, or may
// never arrive at all. What was actually delivered is knowable only from the
// request's completion, which this provider does not yet resolve.
//
// Keyed by WORKER rather than endpoint on purpose. ucp_tag_recv_nbx matches by
// tag across every peer the worker can hear from, so there is no endpoint to
// name, and inventing one by guessing the likely sender would be a fabricated
// join that reads exactly like a measured one.
PCOMM_RECORD(UcxTagRecv, RecordKind::Event, 608, "ucx_tag_recv") {
    Handle        worker;
    Handle        buffer;
    Handle        request;
    std::uint64_t count;  // capacity OFFERED, not bytes delivered
    std::uint64_t tag;
    std::uint64_t tag_mask;  // which bits of `tag` the match tests
    // Set ONLY on the ucp_tag_msg_recv_nb path (door = LegacyMsg), where it is
    // the interned ucp_tag_message_h and `tag`/`tag_mask` are 0 because that
    // door genuinely has neither — it receives a message already matched by an
    // earlier probe. Join it to ucx_tag_probe.message to recover the tag AND
    // the true delivered length.
    //
    // THAT JOIN MUST BE TEMPORAL, not a plain equality: the handle is a pointer
    // into UCX's request mpool and is recycled, which was observed directly
    // while validating this design (UCX printed its own mpool-reuse warning in
    // the same run). "The most recent probe with this handle at or before this
    // receive" is the rule; matching on the value alone will pair a receive with
    // a long-dead probe as soon as the pool wraps.
    Handle       message;
    std::int32_t status;
    std::uint8_t status_class;
    std::uint8_t door;  // UcxDoor
};

// EVENT — a probe that FOUND a message. Probes that find nothing are dropped at
// the hook, for the reason ibv_poll_cq drops empty polls: a probe loop spins.
//
// This record is worth having beyond its role as the tag source for
// ucp_tag_msg_recv_nb, because `length` is the one TRUE byte count anywhere in
// the UCX record family. Every other size here is capacity offered or a raw
// datatype count; this one is how big the message actually is, straight out of
// ucp_tag_recv_info_t, known before the receive is even posted.
PCOMM_RECORD(UcxTagProbe, RecordKind::Event, 618, "ucx_tag_probe") {
    Handle        worker;
    Handle        message;     // interned ucp_tag_message_h — the join key
    std::uint64_t sender_tag;  // the FULL tag of the matched message
    std::uint64_t length;      // TRUE message bytes, not a capacity
    std::uint64_t tag;         // what the caller asked for
    std::uint64_t tag_mask;
    std::uint8_t  removed;  // the `remove` argument: 1 = dequeued for msg_recv
};

// Which one-sided operation a UcxRma row describes. Values are pinned: they go
// on disk. Put and get share a record for the reason ib_post_send covers send,
// RDMA write and RDMA read in one — the question "what one-sided traffic did
// this endpoint issue" should not require a union of two tables — and they are
// discriminated here rather than by table because the direction is the only
// thing that differs between them.
enum class RmaOp : std::uint8_t {
    Put = 0,  // local buffer -> remote_addr
    Get = 1,  // remote_addr -> local buffer
};

// EVENT — a one-sided put or get was issued. `remote_addr` is the peer's
// virtual address, and it is the field that makes these rows joinable to a
// remote object the way ib_post_send.remote_addr already is.
PCOMM_RECORD(UcxRma, RecordKind::Event, 610, "ucx_rma") {
    Handle        endpoint;
    Handle        buffer;  // the LOCAL buffer
    Handle        request;
    Handle        rkey;  // interned ucp_rkey_h; the remote key, not the address
    std::uint64_t count;
    std::uint64_t remote_addr;
    std::int32_t  status;
    std::uint8_t  status_class;
    std::uint8_t  op;    // RmaOp
    std::uint8_t  door;  // UcxDoor
};

// EVENT — an active-message send. The header and the payload keep separate
// lengths rather than being summed: an AM protocol's header is control traffic
// and its payload is data, and a total that merges them cannot be split back.
PCOMM_RECORD(UcxAmSend, RecordKind::Event, 612, "ucx_am_send") {
    Handle        endpoint;
    Handle        buffer;  // the payload buffer; the header is not interned
    Handle        request;
    std::uint32_t am_id;
    std::uint64_t count;          // payload, raw UCP datatype count
    std::uint64_t header_length;  // bytes, and this one really is bytes
    std::int32_t  status;
    std::uint8_t  status_class;
};

// ENTITY — a UCP memory registration, lifetime [ucp_mem_map, ucp_mem_unmap).
//
// Deliberately NOT a MemObject. The engine owns object identity, and a region
// registered here is very often the same memory some other provider already
// named; minting a second identity for it would put two rows in front of a
// reader for one allocation and leave them to guess which is authoritative.
// This record says only "UCX was asked to register this range", and the join to
// an object goes through the address like every other address in the capture.
//
// `address` and `length` are the EFFECTIVE values from ucp_mem_query where it
// answers, and the requested ones from ucp_mem_map_params_t otherwise — the two
// differ whenever UCX rounds to pages or picks the address itself, which is
// exactly what happens when the caller passes UCP_MEM_MAP_ALLOCATE. `resolved`
// says which of the two a row carries, because a rounded length and a requested
// length are different measurements and a reader cannot tell them apart by
// looking.
PCOMM_RECORD(UcxMemMap, RecordKind::Entity, 614, "ucx_mem_map") {
    Handle        memh;  // interned ucp_mem_h
    Handle        context;
    std::uint64_t address;
    std::uint64_t length;
    std::uint8_t  resolved;  // 1 = effective, from ucp_mem_query; 0 = as requested
};

// EVENT — a worker progress call that actually progressed something.
//
// Full mode only, and filtered at the hook: Stage 0 measured 329,073-617,094
// progress calls per rank against 133 tagged sends, varying by a factor of two
// between two runs of the identical workload. So the empty polls are dropped
// (see the predicate on the hook) and what remains is telemetry — never a
// golden, and never evidence about how much work a rank did.
PCOMM_RECORD(UcxWorkerProgress, RecordKind::Event, 616, "ucx_worker_progress") {
    Handle        worker;
    std::uint32_t progressed;  // callbacks dispatched by this call
};

// The atomic operation, NORMALISED. UCX numbers the same operations differently
// in each of its three atomic doors, so the raw opcode is not merely opaque —
// it is ambiguous. Measured from the headers:
//
//   value  ucp_atomic_post_op_t  ucp_atomic_fetch_op_t  ucp_atomic_op_t
//     0    ADD                   FADD                   ADD
//     1    AND                   SWAP                   SWAP
//     2    OR                    CSWAP                  CSWAP
//     3    XOR                   FAND                   AND
//
// So `op = 1` means AND, SWAP and SWAP depending on which door wrote it. Storing
// the raw byte would put a value on disk that cannot be read without also
// reading `door` and a mapping table that lives nowhere — the "plausible-looking
// lie" failure mode that port_state_name already cost this project once. These
// are mapped at the hook instead, and `door` stays for provenance rather than
// for decoding.
enum class UcxAtomicOp : std::uint8_t {
    Add = 0,
    And = 1,
    Or = 2,
    Xor = 3,
    Swap = 4,
    CompareSwap = 5,
    Unknown = 255,  // an opcode this build's UCX headers did not define
};

// EVENT — a remote atomic. Both MPI one-sided (ucp_atomic_post,
// ucp_atomic_fetch_nb) and UCC (ucp_atomic_op_nbx) issue these, so without this
// record an MPI RMA capture is missing the operations that make it RMA.
//
// `fetching` is the difference between an operation that returns the previous
// value and one that does not, which is not recoverable from `op`: ADD is
// fire-and-forget through ucp_atomic_post and fetching through
// ucp_atomic_fetch_nb's FADD, and through ucp_atomic_op_nbx it is whichever the
// caller's reply buffer says. Read from the field_mask there, never assumed.
PCOMM_RECORD(UcxAtomic, RecordKind::Event, 620, "ucx_atomic") {
    Handle        endpoint;
    Handle        buffer;  // operand/result buffer where the door has one
    Handle        request;
    Handle        rkey;
    std::uint64_t value;        // immediate operand (post/fetch doors); 0 for nbx
    std::uint64_t remote_addr;  // the target of the atomic
    std::uint64_t op_size;      // operand bytes — 4 or 8
    std::int32_t  status;
    std::uint8_t  status_class;
    std::uint8_t  op;        // UcxAtomicOp, normalised
    std::uint8_t  door;      // UcxDoor
    std::uint8_t  fetching;  // 1 = returns the previous value
};

// ENTITY — this worker's own address, reduced to hashes. The address BYTES are
// deliberately not recorded: they are tens to hundreds of bytes of transport
// detail per worker, and nothing downstream wants to read them. What is wanted
// is identity — "is the worker this endpoint dials the same one rank 3
// created?" — and a hash answers that at fixed width.
//
// TWO hashes, and the pair is the point. `prefix_hash` covers the first
// kAddressHashPrefix bytes and is the one a join can actually use, because
// ucp_ep_create hands us a remote address pointer with NO LENGTH (see
// UcxEpConnection) — a prefix is all the API makes available on that side.
// `addr_hash` covers the whole address, which only the local side can compute.
//
// Recording both is what makes a prefix collision VISIBLE instead of silently
// wrong: two workers that share a prefix_hash and differ in addr_hash are two
// workers a prefix join cannot tell apart, and a reader can say so. With only
// the prefix stored, the same collision reads as a confident, wrong join.
PCOMM_RECORD(UcxWorkerAddress, RecordKind::Entity, 622, "ucx_worker_address") {
    Handle        worker;
    std::uint64_t addr_hash;    // FNV-1a over the whole address
    std::uint64_t prefix_hash;  // FNV-1a over the first kAddressHashPrefix bytes
    std::uint32_t addr_len;     // whole address bytes, so a short address is visible
};

// ENTITY — which remote worker an endpoint was dialled at, as the hash of the
// address passed to ucp_ep_create. Join to UcxWorkerAddress.prefix_hash from
// ANOTHER RANK's capture: that is what turns a pile of per-rank endpoint rows
// into a communication graph, and it is the only thing in the UCX records that
// crosses a rank boundary at all.
//
// Only the prefix is available here. ucp_ep_params_t carries `address` and no
// length — the caller is expected to know it out of band — so hashing the whole
// remote address is not merely inconvenient, it is unreachable through the
// public API. Hence the two-hash scheme on the worker side.
//
// Emitted only when UCP_EP_PARAM_FIELD_REMOTE_ADDRESS is actually set. An
// endpoint created from a socket address or a connection request has no peer
// address here and gets no row, rather than a row full of zeroes that reads like
// a measured null peer.
PCOMM_RECORD(UcxEpConnection, RecordKind::Entity, 624, "ucx_ep_connection") {
    Handle        endpoint;
    std::uint64_t remote_prefix_hash;
};

}  // namespace pcomm::providers::ucx::records
