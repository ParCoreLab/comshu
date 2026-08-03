-- requires: gpu_access params.ranks
-- Every rank must record both peer loads and peer stores. Counts deliberately
-- absent: NCCL's LL protocol spins on flag polling, so the number of
-- instrumented accesses is genuinely not repeatable — two identical runs
-- recorded 295,044 and 550,064 loads on rank 0, and job 44113361 recorded
-- 295,041 / 556,546 / 493,031 / 507,375 across its four ranks against a
-- perfectly flat 294,990 stores. Any volume assertion here would be a golden in
-- a costume.
--
-- What is invariant is that a rank participating in an NVLink all-reduce both
-- reads a peer's buffer and writes one. A rank that recorded only one direction
-- has lost half its instrumentation, which is the failure a per-rank presence
-- check catches and a total cannot.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'load')  AS loads,
         (SELECT count(*) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store') AS stores
  FROM expected
)
WHERE loads = 0 OR stores = 0
ORDER BY rank;
