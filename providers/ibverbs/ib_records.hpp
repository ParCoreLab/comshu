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
PCOMM_RECORD(IbMemRegion, RecordKind::Entity, 100, "ib_mem_region") {
    Handle        mr;      // interned ibv_mr*
    Handle        pd;      // interned ibv_pd*
    MemRange      range;   // addr, length
    std::uint32_t lkey;
    std::uint32_t rkey;
    std::uint32_t access;
};

// ENTITY — a queue-pair endpoint (ibv_create_qp), lifetime [create, destroy).
PCOMM_RECORD(IbQueuePair, RecordKind::Entity, 102, "ib_queue_pair") {
    Handle        qp;      // interned ibv_qp*
    Handle        pd;      // interned ibv_pd*
    std::uint32_t qp_num;
    std::uint8_t  qp_type;
};

// EVENT — a QP state transition (ibv_modify_qp). The ->RTR transition carries
// the remote endpoint (dest_qp_num/dest_lid), giving the connection topology.
PCOMM_RECORD(IbModifyQp, RecordKind::Event, 104, "ib_modify_qp") {
    Handle        qp;
    std::uint32_t qp_num;
    std::uint32_t dest_qp_num;  // remote QP (valid when moving to RTR)
    std::uint32_t dest_lid;     // remote LID
    std::uint8_t  state;        // new ibv_qp_state
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
    std::uint8_t  opcode;       // ibv_wr_opcode
    std::uint8_t  num_sge;
};

// EVENT — a completion drained from a CQ (ibv_poll_cq); one row per completion.
PCOMM_RECORD(IbPollCq, RecordKind::Event, 112, "ib_poll_cq") {
    Handle        cq;
    std::uint64_t wr_id;
    std::uint32_t byte_len;
    std::uint32_t qp_num;
    std::uint8_t  status;    // ibv_wc_status
    std::uint8_t  opcode;    // ibv_wc_opcode
};

}  // namespace pcomm::providers::ibverbs::records
