-- requires: gpu_access params.gpus params.iters params.elems
-- putget is a ring: each PE puts to exactly one neighbour, (dev + 1) mod gpus,
-- elems ints per iteration. So every remote store lands on that neighbour and
-- each ring edge carries exactly iters x elems of them.
--
-- History worth keeping. A golden once asserted fan-out 2 here and passed for
-- weeks: it had captured an attribution bug — one real edge split across two
-- phantom devices as the fragmented NVSHMEM 3.7 heap slid the positional
-- lookup by one region (jobs 44111484, 44113361) — and turned it into the
-- expected result. This file was written as the failing acceptance test for
-- PLAN-20260803-1448-nvshmem-heap-attribution, went green when that landed, and
-- now replaces the peer_topology golden, which encoded the same ring as
-- numbers from one run.
WITH RECURSIVE devs(d) AS (
  SELECT 0
  UNION ALL SELECT d + 1 FROM devs WHERE d + 1 < (SELECT gpus FROM params)
)
SELECT 'wrong neighbour' AS problem, CAST(run_dev AS INTEGER) AS run_dev,
       CAST(mem_dev AS INTEGER) AS mem_dev, count(*) AS accesses
FROM gpu_access
WHERE run_dev != mem_dev
GROUP BY run_dev, mem_dev
HAVING CAST(mem_dev AS INTEGER) != (CAST(run_dev AS INTEGER) + 1) % (SELECT gpus FROM params)
    OR count(*) != (SELECT iters * elems FROM params)

UNION ALL

SELECT 'ring edge missing', d, (d + 1) % (SELECT gpus FROM params), 0
FROM devs
WHERE NOT EXISTS (
    SELECT 1 FROM gpu_access g
    WHERE CAST(g.run_dev AS INTEGER) = d
      AND CAST(g.mem_dev AS INTEGER) = (d + 1) % (SELECT gpus FROM params));
