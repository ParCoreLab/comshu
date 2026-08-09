-- requires: mlx5_doorbell mlx5_device_send
-- COMPLETENESS of the device data plane, checked against the hardware's own
-- counter rather than against a count we chose.
--
-- prod_idx is the producer index the GPU writes when it rings the doorbell.
-- **It counts work-queue slots, not doorbells** — a fact this file originally
-- got wrong. The first version asserted the sequence was contiguous (0,1,2,...)
-- and it fired on a healthy capture: rank 0's queue pair recorded 51 doorbells
-- across an index span of 1372. Measured on du04 (2026-08-10) the strides are
-- 3, 1, 3, 1, 3, 3, 64, 67 — a WQE occupies several slots, and one doorbell can
-- announce a whole batch of them. 168 doorbells decoded to 1309 sends.
--
-- So contiguity was never the invariant. What IS true of the protocol, and is
-- asserted here:
--
--   1. prod_idx is strictly increasing per queue pair. A repeat means the same
--      doorbell was recorded twice — the hijack forwarding a store it had
--      already accounted for. A decrease means we lost the ordering.
--   2. Every doorbell but the last of a queue pair has a decoded WQE at its
--      index. The trailing one does not, and must not be counted as a loss:
--      the producer index names the next free slot, so the final ring announces
--      an index nothing has been written to yet. Measured: exactly one such
--      doorbell per queue pair, always the last, on all four QPs.
--
-- Both are structural. Neither names a rank, a qp_num or a count from any
-- particular run.
--
-- Deliberately NOT asserted: that the sequence starts at slot 0. The hijack
-- installs when the export table is first patched, so a capture can legitimately
-- begin mid-stream. Starting late is bounded and known; losing a doorbell in the
-- middle is not.
--
-- CAST is not decoration. sql_check.sh loads these CSVs into sqlite, where every
-- column is TEXT, so comparisons and min/max would be lexicographic — "1000"
-- sorts below "999" — which is how the first version of this file managed to
-- report a negative number of missing indices.
SELECT 'duplicate producer index' AS problem,
       d.rank,
       d.qp_num,
       CAST(d.prod_idx AS INTEGER) AS prod_idx,
       count(*)                    AS times_recorded
FROM mlx5_doorbell d
GROUP BY d.rank, d.qp_num, CAST(d.prod_idx AS INTEGER)
HAVING count(*) > 1

UNION ALL

-- A doorbell whose announced slot has no decoded WQE, excluding the trailing
-- one per queue pair. The join is what ties the control plane (the ring) to the
-- data plane (what the ring was announcing) — a doorbell we saw but could not
-- decode is a silent hole in the send record, and the count of sends alone
-- would never show it.
SELECT 'doorbell announced a slot with no decoded WQE' AS problem,
       d.rank,
       d.qp_num,
       CAST(d.prod_idx AS INTEGER) AS prod_idx,
       0                           AS times_recorded
FROM mlx5_doorbell d
WHERE CAST(d.prod_idx AS INTEGER) < (
        SELECT max(CAST(d2.prod_idx AS INTEGER))
        FROM mlx5_doorbell d2
        WHERE d2.rank = d.rank AND d2.qp_num = d.qp_num)
  AND NOT EXISTS (
        SELECT 1 FROM mlx5_device_send s
        WHERE s.rank = d.rank
          AND s.qp_num = d.qp_num
          AND CAST(s.wqe_idx AS INTEGER) = CAST(d.prod_idx AS INTEGER));
