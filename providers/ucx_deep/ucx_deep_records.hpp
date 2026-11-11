#pragma once
// UCT records (800-899) — UCX's TRANSPORT layer, one call above the WQE.
//
// The 600-699 family records what the application ASKED UCX for. This family
// records what UCX then did about it, and on the HPC-X/mlx5 stack it is the
// only layer of comshu that sees the data plane at all: the 2026-08-27 door
// sweep measured `ibv_post_send` and `mlx5_device_send` both empty against
// 24,452 tagged sends, because UCX builds its WQE and rings its own UAR inside
// a function nobody exports.
//
// WHY THESE EXIST AS A SEPARATE PROVIDER RANGE. The 600s go through public UCP
// symbols and are as stable as UCX's application API. These go through
// `uct_iface_ops_t`, the transport-implementor table in `uct/api/tl.h` — an
// INSTALLED header, so no offset is guessed and no pointer is walked, but one
// that carries no ABI promise across versions. That is a different risk class
// and PLAN-20260815-1540 reserved 800-899 for exactly it. `uct_binding` below
// makes each capture state what it bound to, so the risk is auditable from the
// data rather than from this comment.
//
// NO UCX HEADER IS INCLUDED HERE, deliberately and by the same rule
// ib_records.hpp and ucx_records.hpp follow: the reader and the schema
// conformance test compile these records on hosts with no UCX at all.

#include <cstdint>

#include "comshu/core/record.hpp"
#include "comshu/core/vocab.hpp"

namespace comshu::providers::ucx_deep::records {
using namespace comshu::core;

// WHICH SLOT of uct_iface_ops_t carried this operation. Values are pinned: they
// go on disk.
//
// This is not a restatement of `ucx_tag_send.door`. That field says which entry
// point the APPLICATION called; this says which of the transport's fourteen
// data primitives UCX chose underneath, and the choice is the protocol
// decision — short vs bcopy vs zcopy is inline-in-the-WQE vs copy-to-a-bounce-
// buffer vs register-and-DMA, and it is what turns one UCP send into one, two
// or many wire operations.
//
// The three FAMILIES differ in what `payload` means, and a reader must not sum
// across them:
//   Am*, Put*, TagEager*   payload LEFT this node during the call.
//   Get*                   payload was REQUESTED; it arrives later.
//   TagRndvZcopy           payload was ADVERTISED for the peer to pull; the
//                          call itself puts only `header` bytes on the wire.
enum class UctOp : std::uint8_t {
    AmShort = 0,
    AmShortIov = 1,
    AmBcopy = 2,
    AmZcopy = 3,
    PutShort = 4,
    PutBcopy = 5,
    PutZcopy = 6,
    GetShort = 7,
    GetBcopy = 8,
    GetZcopy = 9,
    TagEagerShort = 10,
    TagEagerBcopy = 11,
    TagEagerZcopy = 12,
    TagRndvZcopy = 13,
};

// Why an interface was, or was not, instrumented. On disk.
enum class UctInstrumented : std::uint8_t {
    No = 0,           // the guard refused; ops left untouched
    Yes = 1,          // data slots patched
    NoDataSlots = 2,  // guard passed, but this transport fills none of them
};

// ENTITY — one UCT interface: a (transport, device) pair opened on a worker.
//
// `tl_name`/`dev_name` are the fact this family exists to add. UCP knows only
// that it sent something; the interface knows it went over `rc_mlx5` on
// `mlx5_0:1`, or `cuda_ipc`, or `posix` — that is, whether the message left the
// node at all. No other record in comshu can answer that for a UCX application,
// and `comm_edges` has been counting shared-memory traffic as fabric traffic
// for want of it.
//
// Both names come from `uct_iface_params_t.mode.device`, and are read ONLY when
// `UCT_IFACE_PARAM_FIELD_DEVICE` is in the field mask AND
// `UCT_IFACE_OPEN_MODE_DEVICE` is in `open_mode` — a client-server interface is
// opened by socket address and genuinely has neither, where kNullHandle is the
// truthful answer and an empty string would read as a measured empty name.
COMSHU_RECORD(UctIface, RecordKind::Entity, 800, "uct_iface") {
    std::uint64_t open_mode;  // uct_iface_open_mode bitmap, verbatim
    Handle        iface;
    Handle        worker;  // uct_worker_h
    Handle        md;      // uct_md_h — the memory domain the registrations belong to
    Handle        tl_name;
    Handle        dev_name;
    std::uint8_t  instrumented;  // UctInstrumented
    std::uint8_t  patched_ops;   // how many of the 14 data slots were non-NULL and patched
    std::uint8_t  pad0;
    std::uint8_t  pad1;
};

// ENTITY — one UCT endpoint. THIS IS THE LANE.
//
// A ucp_ep fans out over several uct_eps: one per lane, and lanes may be on
// different transports (an rc_mlx5 lane for the fabric, a posix lane for a peer
// on the same node). PLAN-20260815-1540 said "an endpoint may use several lanes
// and transports, so even a correct static endpoint -> one QP row can be
// false"; this is the record that makes the fan-out visible instead of
// collapsing it.
//
// It is NOT a QP. `uct_ep_get_address` would pack a QP number for the RC
// transports, but only through `uct_rc_mlx5_ep_address_t`, an internal struct —
// the exact thing this project refuses to parse. The public `uct_ep_query` does
// not expose one. So the lane identity here is the uct_ep pointer, and a
// capture that wanted a QP number must say it does not have one.
COMSHU_RECORD(UctEndpoint, RecordKind::Entity, 802, "uct_ep") {
    Handle ep;
    Handle iface;
};

// EVENT — one UCT data operation. The wire, as close to it as a host-side
// function interposition can get.
//
// `payload` and `header` are kept apart for the reason UcxAmSend keeps them
// apart: a header is the protocol's own bytes and a payload is the
// application's, and summing them makes a bandwidth figure that is neither.
// `header` is 8 on AmShort (UCT's inline header is a uint64 by signature),
// the stated header length on AmZcopy and TagRndvZcopy, and 0 elsewhere.
//
// `payload` is measured differently per door and the difference is real:
//   *Short           the `length` argument.
//   *Bcopy           the RETURN value, which is the number of bytes the pack
//                    callback actually produced — so it is known only after the
//                    call, and is negative on failure, in which case it is 0
//                    here and `status` carries the error.
//   *Zcopy           the sum over the iov, which is why `iovcnt` rides along:
//                    a reader can tell one 64 KB buffer from 64 scattered ones.
//
// `tag` is set only by the TagEager*/TagRndv* doors, which are HW tag matching
// — the ConnectX offload. It is the join back to `ucx_tag_send.tag`, and it is
// 0 (not "unknown") on every other door because those doors carry no tag at
// all: on a stack without offload, UCP does its own tag matching and the tag
// travels inside an AM payload this record does not parse.
//
// `remote_addr` is set only by Put*/Get*. It is the same object-heatmap key
// `ib_post_send.remote_addr` provides for raw verbs, and until now the whole
// HPC-X class had none.
COMSHU_RECORD(UctSend, RecordKind::Event, 804, "uct_send") {
    std::uint64_t payload;
    std::uint64_t header;
    std::uint64_t remote_addr;  // Put*/Get* only, else 0
    std::uint64_t tag;          // HW tag-matching doors only, else 0
    Handle        ep;
    Handle        iface;
    std::int32_t  status;  // ucs_status_t; UCS_OK(0) on the bcopy doors' success
    std::uint8_t  op;      // UctOp
    std::uint8_t  iovcnt;  // *Zcopy only, saturating at 255; else 0
    std::uint8_t  pad0;
    std::uint8_t  pad1;
};

// ARTIFACT — which UCX this provider bound itself to, and whether it dared.
//
// The whole family rests on `uct_iface_ops_t` having the layout we compiled
// against. That was measured stable (byte-identical `tl.h` from v1.14.0 to
// origin/master) but it is not promised, so a capture must not require anyone
// to remember the measurement. One row, written once, says: this is the UCX we
// compiled against, this is the UCX that was loaded, this is what we thought
// the table's size was, and this is whether the guard let us patch.
//
// `accepted = 0` is the honest empty capture: `uct_send` will have no rows and
// this row says why, instead of leaving a reader to conclude the application
// sent nothing.
COMSHU_RECORD(UctBinding, RecordKind::Artifact, 806, "uct_binding") {
    std::uint32_t compiled_version;  // (major << 16) | (minor << 8) | patch
    std::uint32_t runtime_version;   // from ucp_get_version(), same packing
    std::uint32_t ops_size;          // sizeof(uct_iface_ops_t) as compiled
    std::uint8_t  accepted;          // 1 = the guard allowed patching
    std::uint8_t  forced;            // 1 = COMSHU_UCX_DEEP_FORCE overrode a refusal
    std::uint8_t  pad0;
    std::uint8_t  pad1;
};

}  // namespace comshu::providers::ucx_deep::records
