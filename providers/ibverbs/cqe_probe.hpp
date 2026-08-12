#pragma once
// PROBE — is the raw mlx5 CQE still readable, and still CORRECT, once
// ibv_poll_cq has returned?
//
// THE QUESTION, and why it is worth a job.
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
// Enabled by PCOMM_CQE_PROBE=1. Off, this costs one branch per poll.
// POSITIVE CONTROL: any [pcomm-cqeprobe] line at all. A run with none did not
// execute this code, and nothing it did not print is a finding.

//
// The IMPLEMENTATION is in cqe_probe.cpp and not here, and that is forced
// rather than stylistic: ibverbs_provider.cpp defines interposers for
// mlx5dv_open_device and mlx5dv_create_cq with their mlx5-specific attribute
// arguments typed `void*`, precisely so that it need not include mlx5dv.h.
// Including the real header in that translation unit makes those definitions
// conflicting declarations of a C function and the build fails. So the probe
// keeps mlx5dv.h to itself, and the provider sees only this declaration.

#include <infiniband/verbs.h>

namespace ibv::cqeprobe {

/** True when PCOMM_CQE_PROBE names a non-zero value. Read once. */
bool enabled();

/**
 * One poll's worth of completions, checked against the CQEs they came from.
 * Cheap and safe to call unconditionally: returns on the first branch when the
 * probe is off, which is the default.
 */
void on_poll(ibv_cq* cq, int ret, ibv_wc* wc);

}  // namespace ibv::cqeprobe
