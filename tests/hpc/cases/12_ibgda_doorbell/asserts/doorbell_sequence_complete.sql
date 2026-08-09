-- requires: mlx5_doorbell
-- COMPLETENESS, checked against the hardware's own counter rather than against
-- a count we chose.
--
-- prod_idx is the producer index the GPU writes when it rings the doorbell: a
-- per-queue-pair monotonic sequence number, not a timestamp. So the doorbells
-- a queue pair rang are 0,1,2,...,N with no gaps, by construction of the
-- protocol — and a gap in what we recorded is a doorbell we MISSED.
--
-- This is why it is worth more than a count. "We recorded 4096 doorbells" can
-- only be checked against an oracle for how many there should have been; a gap
-- is self-evidently wrong and says exactly where the loss began. It is also
-- the one completeness claim on the device data plane that needs no second
-- tool: it is the reason the SigmaTrace parity plan
-- (PLAN-20260810-0102-sigmatrace-parity) puts this step before any comparison.
--
-- Offending rows: one per queue pair whose recorded sequence is not contiguous,
-- with the size of the hole. Names no specific qp_num and no count from any
-- particular run — it is a structural property of every capture.
--
-- Deliberately NOT asserted here: that the sequence starts at 0. A capture can
-- legitimately begin mid-stream, since the hijack installs when the export
-- table is first patched and the application may have rung doorbells before
-- that. Starting late is a known and bounded thing; a hole in the middle is
-- not.
WITH seq AS (
    SELECT rank,
           qp_num,
           count(*)                       AS recorded,
           min(prod_idx)                  AS lo,
           max(prod_idx)                  AS hi,
           count(DISTINCT prod_idx)       AS distinct_idx
    FROM mlx5_doorbell
    GROUP BY rank, qp_num
)
SELECT 'gap in the doorbell sequence' AS problem,
       rank,
       qp_num,
       recorded,
       (hi - lo + 1) - distinct_idx      AS missing
FROM seq
WHERE distinct_idx <> (hi - lo + 1)

UNION ALL

-- A repeated producer index means the same doorbell was recorded twice — the
-- hijack forwarding a store it had already accounted for. The opposite failure
-- to a gap, and equally invisible to a count.
SELECT 'duplicate producer index' AS problem,
       rank,
       qp_num,
       recorded,
       recorded - distinct_idx           AS missing
FROM seq
WHERE recorded <> distinct_idx;
