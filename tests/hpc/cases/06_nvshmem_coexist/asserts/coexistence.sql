-- requires: gpu_access ib_queue_pair ib_mem_region params.ranks
-- THE assertion of this case: both providers recorded into ONE capture, on the
-- same rank space, from ONE execution. Every rank must show up with both GPU
-- accesses and IB entities. Under replay this row shape is unreachable — the
-- providers would be two separate runs writing two separate rank spaces.
--
-- Count-free on the ib side deliberately. Single-node NVSHMEM builds its IB
-- transport but never posts a send (every peer is NVLink-reachable), so what
-- the ib provider contributes here is entities, not traffic; ib_post_send and
-- ib_poll_cq are legitimately empty. Volume lives in the goldens.
--
-- The failure mode is a whole provider going quiet — weak-symbol interposition
-- breaking again — which the golden form of this check reports as four rows
-- turning into four different rows. Here it reports as the ranks that lost a
-- provider, by name of the provider.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM gpu_access    g WHERE CAST(g.rank AS INTEGER) = r) AS gpu_rows,
         (SELECT count(*) FROM ib_queue_pair q WHERE CAST(q.rank AS INTEGER) = r) AS qp_rows,
         (SELECT count(*) FROM ib_mem_region m WHERE CAST(m.rank AS INTEGER) = r) AS mr_rows
  FROM expected
)
WHERE gpu_rows = 0 OR qp_rows = 0 OR mr_rows = 0
ORDER BY rank;
