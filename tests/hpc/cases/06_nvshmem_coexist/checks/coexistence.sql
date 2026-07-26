-- THE assertion of this case: both providers recorded into ONE capture, on the
-- same ranks, from ONE execution. Under replay this row shape is unreachable —
-- the providers would be two separate runs writing two separate rank spaces.
--
-- Count-free on the ib side deliberately. Single-node NVSHMEM builds its IB
-- transport but never posts a send (every peer is NVLink-reachable), so what
-- the ib provider contributes here is *entities*, not traffic. Asserting
-- presence keeps this a structural invariant; ib entity volume is covered by
-- ib_entities.sql and NVLink volume by traffic.sql.
SELECT rank,
       max(has_gpu) AS gpu,
       max(has_ib)  AS ib
FROM (
  SELECT rank, 1 AS has_gpu, 0 AS has_ib FROM gpu_access
  UNION ALL
  SELECT rank, 0 AS has_gpu, 1 AS has_ib FROM ib_queue_pair
)
GROUP BY rank
ORDER BY CAST(rank AS INTEGER);
