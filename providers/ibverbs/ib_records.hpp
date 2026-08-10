#pragma once
// pcomm ibverbs records (id range 100-199, UNIFICATION §3.4). Vendor-neutral:
// the provider translates ibv_* handles/enums into these before emitting, so
// nothing here includes infiniband/verbs.h. High-cardinality identities
// (mr/qp/cq/pd pointers) are interned Handles (InternDomain::Pointer); the
// small enums (opcode, state, status) stay inline as raw bytes and are
// decoded by pcomm-read.

#include <cstdint>

#include "pcomm/core/ids.hpp"
#include "pcomm/core/record.hpp"
#include "pcomm/core/vocab.hpp"

namespace pcomm::providers::ibverbs::records {

using core::Handle;
using core::MemRange;
using core::RecordKind;

// ENTITY — a registered memory region (ibv_reg_mr), lifetime [reg, dereg).
// Which entry point produced a region. Values are pinned: they go on disk.
enum class IbRegionOrigin : std::uint8_t {
    Unknown = 0,
    RegMr = 1,        // ibv_reg_mr / ibv_reg_mr_iova2 — has a host address
    Dmabuf = 2,       // ibv_reg_dmabuf_mr — address recoverable only sometimes
    DeviceMemory = 3, // ibv_reg_dm_mr — on-NIC memory, NO host address exists
    NullMr = 4,       // ibv_alloc_null_mr — no memory at all, by definition
};

PCOMM_RECORD(IbMemRegion, RecordKind::Entity, 100, "ib_mem_region") {
    Handle        mr;     // interned ibv_mr*
    Handle        pd;     // interned ibv_pd*
    MemRange      range;  // addr, length
    std::uint32_t lkey;
    std::uint32_t rkey;
    std::uint32_t access;
    // -1 for an ordinary registration; otherwise the dmabuf descriptor the
    // buffer was named by (ibv_reg_dmabuf_mr). It marks the rows whose `range`
    // may be unresolvable: a dmabuf MR has no host address of its own, and
    // whether one can be recovered depends on the caller — NCCL passes the real
    // address as `iova` on its net_ib path (reg.cc:35) and 0 on its GDAKI path
    // (gin_host_gdaki.cc:131). registration_has_object exempts a row with no
    // resolvable address rather than us inventing one.
    std::int32_t dmabuf_fd;
    // WHICH DOOR registered this region. Not decoration: it is what lets an
    // assertion tell "we could not resolve this address" from "this region has
    // no host address by construction", which base=0 alone cannot say.
    //
    // Earned by measurement. 474 closes per run named regions nothing opened,
    // all with addr=NULL — 355 at 32 bytes from uct_ib_dereg_mr and 120 at 2048
    // from uct_rc_mlx5_iface_common_dm_cleanup (MN5 job 44368870). DM is
    // DEVICE memory: it lives on the NIC, so a host address does not exist for
    // it and never will. Neither door appears in any library's ELF because both
    // ibv_reg_dm_mr and ibv_alloc_null_mr are `static inline` over
    // verbs_context function pointers (verbs.h:2807, 2830) — the same shape as
    // ibv_create_qp_ex, which cost case 08 a week for the same reason.
    // Stored as the underlying uint8, like Mlx5Umem::binding: a record field's
    // type is part of the on-disk format, and the schema checker only maps
    // scalars and shared vocabulary — a provider-local enum is neither. The
    // enum above is still what call sites use; pcomm-read renders the name.
    std::uint8_t origin;
};

// ENTITY — a queue-pair endpoint (ibv_create_qp), lifetime [create, destroy).
PCOMM_RECORD(IbQueuePair, RecordKind::Entity, 102, "ib_queue_pair") {
    Handle        qp;  // interned ibv_qp*
    Handle        pd;  // interned ibv_pd*
    std::uint32_t qp_num;
    std::uint8_t  qp_type;
};

// ENTITY — a completion queue (ibv_create_cq / ibv_create_cq_ex), lifetime
// [create, destroy).
//
// This record exists because IbPollCq.cq referenced an entity NOTHING CREATED.
// ib_poll_cq is the oldest event table in the capture and its subject column
// joined to nowhere — the same defect entity_close_accounted.sql exists to
// catch, sitting in plain sight because that assertion checks closes against
// opens and nothing checked an *event's* subject against an open.
//
// A CQ hangs off a CONTEXT, not a PD, which is why `context` is here and no
// `pd` is: it is the one ibverbs object that can exist before any protection
// domain does. That is also why ibv_create_cq was already an install door (see
// ib_hooks.hpp) before it ever carried a record.
//
// `channel` is the completion channel, or a null Handle when the caller passed
// none. Not decoration: a CQ with a channel is one the application can *sleep*
// on, so together with ib_req_notify_cq it is what distinguishes an
// interrupt-driven completion path from a polling one — a distinction that
// changes how a latency gap in the capture should be read.
// Which door created a completion queue. Values are pinned: they go on disk.
//
// Added after the first measurement, and it is an instrument rather than
// decoration. Case 02 and case 07 each close exactly 30 CQs per rank that
// nothing created — uniform across all 8 ranks of two unrelated workloads —
// and with one row type for both doors there was no way to ask whether
// create_cq_ex fires at all. IbRegionOrigin exists for precisely this reason
// and the lesson was not applied here the first time.
enum class IbCqOrigin : std::uint8_t {
    Unknown = 0,
    CreateCq = 1,    // ibv_create_cq — the exported symbol, a preload hook
    CreateCqEx = 2,  // verbs_context::create_cq_ex — object-ops, needs install
    // The third door the two above were added to look for, and it is in
    // libmlx5 rather than libibverbs: an exported symbol, so a preload hook.
    // UCX's mlx5 transport creates its tx and rx CQs here. See ib_hooks.hpp for
    // the measurement that found it.
    Mlx5dvCreateCq = 3,
};

PCOMM_RECORD(IbCompletionQueue, RecordKind::Entity, 104, "ib_completion_queue") {
    Handle        cq;       // interned ibv_cq* (== ibv_cq_ex*, see below)
    Handle        context;  // interned ibv_context*
    Handle        channel;  // interned ibv_comp_channel*, null when none
    std::uint32_t cqe;      // the depth the DEVICE granted, from cq->cqe
    std::uint32_t comp_vector;
    // WHICH DOOR, and the distinction is load-bearing: create_cq is a preload
    // interposer that is live from process start, while create_cq_ex is an
    // object-ops slot that only exists once the context is installed. A deficit
    // that is entirely CreateCq rules the install window out; one that is
    // entirely CreateCqEx implicates it; neither means a third door.
    std::uint8_t  origin;
};

// EVENT — a QP state transition (ibv_modify_qp). The ->RTR transition carries
// the remote endpoint (dest_qp_num/dest_lid), giving the connection topology.
PCOMM_RECORD(IbModifyQp, RecordKind::Event, 106, "ib_modify_qp") {
    Handle        qp;
    std::uint32_t qp_num;
    std::uint32_t dest_qp_num;  // remote QP  (set only under IBV_QP_DEST_QPN)
    std::uint32_t dest_lid;     // remote LID (set only under IBV_QP_AV)
    // Our own port's LID. Without it a QP has no globally unique name: qp_num
    // is allocated per HCA, so two ranks on different nodes routinely get the
    // same one (199 collisions in a single 8-rank NCCL capture, job 44139911),
    // and a reader keying peers on qp_num alone attributes edges to whichever
    // rank it happened to scan last — including impossible self-edges. With the
    // LID, (src_lid, qp_num) is unique: qp_num IS unique within one HCA.
    //
    // Learned at the INIT transition, the only one that names a port, and
    // carried on every later transition of that QP. 0 = not yet known.
    std::uint32_t src_lid;
    std::uint8_t  state;  // new ibv_qp_state
};

// EVENT — a work request posted to a QP (ibv_post_send). The comm firehose;
// one row per work request in the posted chain.
PCOMM_RECORD(IbPostSend, RecordKind::Event, 110, "ib_post_send") {
    Handle        qp;
    std::uint64_t wr_id;
    std::uint64_t remote_addr;  // RDMA ops only (0 otherwise)
    std::uint32_t rkey;         // RDMA ops only
    std::uint32_t length;       // total sg_list bytes
    std::uint32_t qp_num;
    std::uint8_t  opcode;  // ibv_wr_opcode
    std::uint8_t  num_sge;
};

// ENTITY — a shared receive queue (ibv_create_srq / ibv_create_srq_ex),
// lifetime [create, destroy).
//
// Comes WITH the SRQ receive path rather than after it, and not optionally:
// without it every ib_post_recv row of origin `srq` would name an entity
// nothing created — the exact defect ib_poll_cq.cq had for as long as it
// existed, and which stage 1 of this plan was spent removing. Adding a subject
// column and its entity in one change is the only way that hole never opens.
//
// The three attrs are the DEVICE's answers, read back with ibv_query_srq rather
// than copied from the caller's request, on the same reasoning that makes
// IbCompletionQueue.cqe come from cq->cqe: the caller states a minimum and the
// device grants at least it, so the argument is a request and the queried value
// is the fact. When the query fails the requested values are kept, because a
// row with approximate depths is worth more than no row at all.
PCOMM_RECORD(IbSharedRecvQueue, RecordKind::Entity, 108, "ib_shared_recv_queue") {
    Handle        srq;  // interned ibv_srq*
    Handle        pd;   // interned ibv_pd*
    std::uint32_t max_wr;
    std::uint32_t max_sge;
    std::uint32_t srq_limit;
};

// Which door posted a receive, and therefore what `subject` points at. Values
// are pinned: they go on disk.
//
// This exists so ibv_post_recv and ibv_post_srq_recv produce ONE record type
// rather than two. A reader asking "what receives were posted" should not have
// to know which API posted them — exactly as it does not have to know whether a
// region came through reg_mr or reg_mr_iova2 (IbRegionOrigin, above). The
// discriminator is here from the record's first version on purpose: a record's
// layout is immutable once a capture containing it exists, so a field that is
// certainly needed later cannot be added later without a version bump.
enum class IbRecvOrigin : std::uint8_t {
    Unknown = 0,
    QueuePair = 1,        // ibv_post_recv — subject is an ibv_qp*, qp_num is set
    SharedRecvQueue = 2,  // ibv_post_srq_recv — subject is an ibv_srq*, no qp_num
};

// EVENT — a work request posted to a receive queue (ibv_post_recv /
// ibv_post_srq_recv). One row per work request, like IbPostSend.
//
// THE RECEIVE SIDE WAS INFERRED AND NEVER OBSERVED. pcomm hooked post_send and
// poll_cq and no receive door at all, so every receive was known only from the
// completion it produced. Case 01's committed golden shows the asymmetry: 20
// recv completions per rank, and nothing anywhere saying which buffer any of
// them landed in. Three things that could not be asked before this record:
//
//   - a receive buffer joins to the ib_mem_region that owns it;
//   - wr_id on a recv completion joins to something, as it already did on a
//     send completion via ib_post_send;
//   - a receive that was never posted is distinguishable from one that
//     completed, where before the only evidence of either was the completion
//     that did or did not arrive.
//
// Two-sided transports are where this matters: NCCL's and NVSHMEM's proxy paths
// post receives ahead of time, and a capture that cannot see them cannot tell a
// stall that was "no buffer posted" from one that was "nothing sent".
//
// `addr`/`lkey` are the FIRST sge's, and they are the reason this record is not
// a copy of SigmaTrace's. SigmaTrace's post_recv handler keeps num_sge and no
// sge list (Profiler/Handlers/IBV/Ibverbs.cpp:179-209), so it never resolves the
// receive buffer either — matching it exactly would have left the central
// question, *which memory receives this*, unanswered. One sge covers the real
// cases (a receive is nearly always a single buffer) and `num_sge` says when it
// does not, which is the same bargain IbPostSend strikes with `length`.
PCOMM_RECORD(IbPostRecv, RecordKind::Event, 114, "ib_post_recv") {
    Handle        subject;  // interned ibv_qp* or ibv_srq* — see origin
    std::uint64_t wr_id;
    std::uint64_t addr;     // first sge's addr; 0 when num_sge == 0
    std::uint32_t lkey;     // first sge's lkey — joins to ib_mem_region.lkey
    std::uint32_t length;   // total sg_list bytes, the capacity being offered
    std::uint32_t qp_num;   // 0 when origin is SharedRecvQueue: an SRQ has none
    std::uint8_t  num_sge;
    std::uint8_t  origin;   // IbRecvOrigin, stored as the underlying uint8
};

// ENTITY — a UD address handle (ibv_create_ah), lifetime [create, destroy).
//
// The addressing object of the UNRELIABLE DATAGRAM path, and the reason a UD
// workload is topologically silent today: comm_edges derives its peers from RC's
// modify_qp->RTR transitions, which carry dest_qp_num and dest_lid. UD's RTR
// carries neither — it needs only IBV_QP_STATE plus pkey/qkey — so a UD run
// produces no edges at all. The destination a UD send is aimed at lives HERE,
// in the ah_attr handed to ibv_create_ah, and nowhere else in the API.
//
// That is not hypothetical for this suite: case 02's NCCL run modifies 240 UD
// QPs to RTR against 24 RC ones (job 44139911). Ten times more of the fabric is
// UD than RC, and all of it is currently invisible to the topology.
//
// GIDs are stored HOST-ORDER, converted with be64toh on the way in, matching
// Mlx5DevxQpConnect.dest_gid_prefix. A wire-order GID in a capture is a number
// that compares unequal to itself across two readers.
PCOMM_RECORD(IbAddressHandle, RecordKind::Entity, 118, "ib_address_handle") {
    Handle        ah;  // interned ibv_ah*
    Handle        pd;  // interned ibv_pd*
    std::uint64_t dgid_prefix;  // remote GID, high 8 bytes (0 unless is_global)
    std::uint64_t dgid_iface;   // remote GID, low 8 bytes
    std::uint32_t flow_label;
    std::uint32_t dlid;
    std::uint8_t  sl;
    std::uint8_t  port_num;
    std::uint8_t  is_global;  // whether the GRH above carries anything
    std::uint8_t  sgid_index;
    std::uint8_t  hop_limit;
    std::uint8_t  traffic_class;
};

// ENTITY — the HCA a context speaks through, one row per ibv_context.
//
// What lets two ranks on different nodes be joined by DEVICE rather than by
// LID. IbModifyQp.src_lid exists because qp_num alone collides across HCAs
// (199 collisions in a single 8-rank NCCL capture); node_guid is the stronger
// form of the same fact, being globally unique by construction rather than
// unique per subnet.
//
// Emitted from ensure_installed rather than from a hooked ibv_query_device, and
// that is the load-bearing choice here. Hooking the door would make the row
// appear only when the APPLICATION happens to ask — most do not, and the ones
// that do, do it once at startup on one context out of dozens. ensure_installed
// runs exactly once per context by construction, so every context gets exactly
// one row. The door records the fact; it does not wait to be told it.
PCOMM_RECORD(IbDevice, RecordKind::Entity, 120, "ib_device") {
    Handle        context;  // interned ibv_context* — the subject
    Handle        fw_ver;   // interned string
    std::uint64_t node_guid;       // host order, see IbAddressHandle
    std::uint64_t sys_image_guid;  // host order
    std::uint32_t vendor_id;
    std::uint32_t vendor_part_id;
    std::uint32_t hw_ver;
    std::uint8_t  phys_port_cnt;
};

// EVENT — a CQ armed for a completion notification (ibv_req_notify_cq).
//
// The ONLY evidence in a capture that an application is waiting on interrupts
// rather than spinning. Without it an interrupt-driven completion path and a
// quiet polling one look identical — a long gap between a post and its
// completion reads as "the network was slow" in both, when in one of them the
// thread was asleep by design and the gap is not latency at all.
//
// Cheap: a CQ is armed once per completion batch, not once per completion, so
// this is orders of magnitude below ib_poll_cq. `cq` joins to
// ib_completion_queue, which stage 1 of this plan made possible.
//
// Note SigmaTrace hooks this door and records nothing — IbvReqNotifyCq is an
// enum entry with no struct behind it. Copying that shape would have bought a
// dlsym chain and an empty table.
PCOMM_RECORD(IbReqNotifyCq, RecordKind::Event, 116, "ib_req_notify_cq") {
    Handle       cq;
    std::uint8_t solicited_only;
};

// EVENT — a completion drained from a CQ (ibv_poll_cq); one row per completion.
PCOMM_RECORD(IbPollCq, RecordKind::Event, 112, "ib_poll_cq") {
    Handle        cq;
    std::uint64_t wr_id;
    std::uint32_t byte_len;
    std::uint32_t qp_num;
    std::uint8_t  status;  // ibv_wc_status
    std::uint8_t  opcode;  // ibv_wc_opcode
};

}  // namespace pcomm::providers::ibverbs::records
