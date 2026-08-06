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
