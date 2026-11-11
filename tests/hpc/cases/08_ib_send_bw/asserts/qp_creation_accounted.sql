-- requires: ib_modify_qp
-- Every queue pair that gets driven must have been recorded being created.
--
-- This is the invariant case 08 was really about. Its `tables_present` marker
-- said ib_queue_pair was empty for perftest and that "the marker comes off when
-- the hook lands, rather than the expectation being edited down to what we
-- emit" — right in spirit, wrong in one detail: perftest genuinely never calls
-- ibv_create_qp, so ib_queue_pair staying empty for THIS workload is correct,
-- not a gap. The gap was that nothing recorded the creation at all.
--
-- So the assertion is the union rather than either table. A queue pair reached
-- through mlx5dv_create_qp is recorded by the mlx5dv provider; one reached
-- through ibv_create_qp by the ibverbs provider. Which door was used is a fact
-- about the application, and pinning either table would test that instead of
-- testing comshu. What must hold, whichever door: a transition cannot refer to a
-- queue pair no record created.
--
-- Deliberately not counting: how many queue pairs perftest builds is its
-- business and varies with its options.
SELECT 'modify_qp on a queue pair nothing recorded creating' AS problem,
       m.rank, m.qp_num
FROM ib_modify_qp m
WHERE NOT EXISTS (SELECT 1 FROM ib_queue_pair q
                   WHERE q.rank = m.rank AND q.qp_num = m.qp_num)
  AND NOT EXISTS (SELECT 1 FROM mlx5_queue_pair k
                   WHERE k.rank = m.rank AND k.qp_num = m.qp_num)
GROUP BY m.rank, m.qp_num;
