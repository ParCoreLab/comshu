#pragma once
// Reading the raw mlx5 CQE, and what that buys.
//
// This file holds two things that grew in that order: a PROBE that answered
// whether the CQE can be read at all, and the PRODUCTION readers that the
// answer made possible. The probe is kept rather than deleted because it is the
// only thing that can re-answer the question when a driver, a firmware or a NIC
// generation changes, and because its cross-check is what the production path
// now uses as a precondition.
//
// THE QUESTION IT ANSWERED, and why it was worth a job.
//
// object_access attributes a two-sided SEND by recovering the receiver's buffer
// through `wr_id`: the completion carries a wr_id, ib_post_recv carries
// (wr_id, addr), and the join gives the address. That works, and on every
// capture measured so far it is unambiguous — but it rests on a key the
// APPLICATION owns. wr_id is caller-chosen and routinely recycled as a buffer
// index, and an SRQ receive can only be keyed on (rank, wr_id), because an SRQ
// WQE is not bound to a queue pair until a message lands.
//
// The hardware does not have that problem. Every mlx5 CQE carries
// `wqe_counter`, the index of the WQE the NIC actually consumed, and
// `srqn_uidx` naming the SRQ it came from. That pair is an exact identity where
// wr_id is a heuristic one.
//
// The sharpest way to see what we are missing: `wqe_counter` is HOW libmlx5
// knows the wr_id in the first place — for an SRQ completion it does
// `wqe_ctr = be16toh(cqe->wqe_counter); wc->wr_id = srq->wrid[wqe_ctr]`. The
// wr_id we read out of ibv_wc is the OUTPUT of a table lookup whose INPUT was
// already exact, and the driver discards the input. We have been reconstructing
// an index the hardware volunteered.
//
// WHAT THIS PROBE ACTUALLY TESTS, which is narrower than "can we use it".
//
// Reading the CQE means reading the driver's own ring after the driver has
// consumed from it. By the time this post-hook runs, ibv_poll_cq has advanced
// the consumer index and written it back to the doorbell record. The CQE bytes
// are still there — a ring entry is only overwritten when the ring wraps, which
// takes cqe_cnt further completions — but "still there" is a claim about a data
// structure this code does not own, and the honest way to settle it is to read
// it on real hardware and check the answer against something already known.
//
// So every CQE read is CROSS-CHECKED against the ibv_wc the driver returned for
// the same completion:
//
//   sop_drop_qpn & 0xffffff  must equal  wc.qp_num
//   byte_cnt                 must equal  wc.byte_len   (responder CQEs only)
//   op_own >> 4              must be     a responder opcode iff wc.opcode is
//
// A high agreement rate means the addressing arithmetic below is right and the
// entry is intact at post-hook time — the cheap version works, and wqe_counter
// can be recorded without hooking deeper. A low or zero rate means the CQE must
// be read from inside the poll path instead, which is a much larger change, and
// it is worth one job to learn which before writing it.
//
// THE ADDRESSING, which is the part most likely to be wrong.
//
// mlx5dv_init_obj(MLX5DV_OBJ_CQ) hands back the ring: buf, cqe_cnt, cqe_size
// and dbrec. libmlx5 indexes it as `get_cqe(cq, n & (cqe_cnt - 1))` with
// `cqe64 = cqe_size == 64 ? cqe : cqe + 64`, and publishes its consumer index
// to `dbrec[MLX5_CQ_SET_CI]` (masked to 24 bits) at the end of each poll. So the
// `ret` completions just reaped occupy indices [ci - ret, ci), and the last 64
// bytes of each slot are the CQE we want.
//
// THE KNOWN WAY THIS BREAKS, stated up front so a clean result is not read as
// more than it is: CQE compression packs several completions into one entry,
// and where it is on the arithmetic above is simply wrong. It is an Ethernet-RQ
// feature and should not appear on RC traffic, but "should not" is why the
// cross-check exists rather than an assertion.
//
// THE ANSWER, so nobody re-runs it to find out: 28,814 CQEs across MN5's
// ConnectX-7 and du04's ConnectX-5, every one agreeing with its ibv_wc on
// qp_num and requester/responder class, and on byte_len wherever a receive was
// involved. The entry survives the driver consuming it, and the addressing
// below is right. Jobs 44542213 / 44542243, and tests/hpc/probes/cqeread.
//
// Enabled by COMSHU_CQE_PROBE=1. Off, this costs one branch per poll.
// POSITIVE CONTROL: any [comshu-cqeprobe] line at all. A run with none did not
// execute this code, and nothing it did not print is a finding.

//
// The IMPLEMENTATION is in mlx5_cqe.cpp and not here, and that is forced
// rather than stylistic: ibverbs_provider.cpp defines interposers for
// mlx5dv_open_device and mlx5dv_create_cq with their mlx5-specific attribute
// arguments typed `void*`, precisely so that it need not include mlx5dv.h.
// Including the real header in that translation unit makes those definitions
// conflicting declarations of a C function and the build fails. So the probe
// keeps mlx5dv.h to itself, and the provider sees only this declaration.

#include <cstdint>

#include <infiniband/verbs.h>

namespace ibv::cqeprobe {

/** True when COMSHU_CQE_PROBE names a non-zero value. Read once. */
bool enabled();

/**
 * One poll's worth of completions, checked against the CQEs they came from.
 * Cheap and safe to call unconditionally: returns on the first branch when the
 * probe is off, which is the default.
 */
void on_poll(ibv_cq* cq, int ret, ibv_wc* wc);

// --- the production path ----------------------------------------------------
//
// The probe above answers "is this readable" and prints. The two below answer
// it for ONE completion and one post, and return the answer for recording.
// Same arithmetic, same cross-check, no printing and no env gate: these run on
// every capture, and where they cannot answer they say so rather than guessing.

/** What the CQE said about a receive, when it could be read and believed. */
struct RecvCqe {
    std::uint32_t srqn = 0;
    std::uint32_t wqe_counter = 0;
    bool          valid = false;  // false: not mlx5, not readable, or disagreed
};

/**
 * Read the CQE behind completion `i` of a batch of `ret`, and believe it only
 * if it agrees with the ibv_wc the driver returned. Callers must not record
 * anything when `valid` is false — a row that might name the wrong WQE is
 * worse than no row, because the whole point of these records is exactness.
 */
RecvCqe read_recv_cqe(ibv_cq* cq, int ret, int i, const ibv_wc& wc);

/** Where the next posted WQE will land in an SRQ ring, and that ring's number. */
struct SrqSlot {
    std::uint32_t srqn = 0;
    std::uint32_t slot = 0;
    bool          valid = false;
};

/**
 * The slot `n` posts ahead of the SRQ's current head, following the ring's own
 * free list — which is how libmlx5 assigns them, and why a post chain does not
 * simply occupy consecutive indices.
 *
 * Must be called BEFORE ibv_post_srq_recv runs: afterwards the head has moved
 * and the slots this call would report are the next post's, not this one's.
 */
SrqSlot srq_slot(ibv_srq* srq, unsigned n);

}  // namespace ibv::cqeprobe
