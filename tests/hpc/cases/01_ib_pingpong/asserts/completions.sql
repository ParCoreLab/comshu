-- requires: ib_poll_cq params.ranks params.iters
-- A pingpong is symmetric: each side sends `iters` messages and receives
-- `iters` messages, so each side must reap `iters` send completions and
-- `iters` recv completions. Anything else means work requests went missing on
-- one of the two halves of the queue pair, which a per-rank total would hide by
-- letting the two errors cancel.
--
-- Status is asserted by suite/asserts/completions_succeed.sql, so this counts
-- only what succeeded — an err* row must not be allowed to make the count up.
--
-- Driven off the workload's rank list rather than off the table, so a rank that
-- reaped nothing at all is an offending row rather than an absent group. See
-- traffic.sql.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM ib_poll_cq c WHERE CAST(c.rank AS INTEGER) = r
          AND c.status = 'success' AND c.opcode = 'send') AS send_cqes,
         (SELECT count(*) FROM ib_poll_cq c WHERE CAST(c.rank AS INTEGER) = r
          AND c.status = 'success' AND c.opcode = 'recv') AS recv_cqes
  FROM expected
)
WHERE send_cqes != (SELECT iters FROM params)
   OR recv_cqes != (SELECT iters FROM params)
ORDER BY rank;
