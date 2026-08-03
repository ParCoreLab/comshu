-- requires: gpu_access params.ranks params.iters params.elems params.elem_bytes
-- Identical oracle to 04_nvshmem, because it is identical to 04's workload:
-- `putget 65536 3` on 4 PEs, elems*iters = 196608 peer stores of 4 B per rank.
-- The one variable this case changes is that the ib provider is loaded into the
-- same process.
--
-- Stating it as a derivation rather than as "the same numbers as case 04" is
-- what makes the comparison mean something. Both cases assert the same claim
-- against the same oracle; if only one of them fails, the second provider
-- perturbed the first, which is the entire reason this case exists.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store') AS stores,
         (SELECT coalesce(sum(CAST(g.size AS INTEGER)), 0) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store') AS bytes
  FROM expected
)
WHERE stores != (SELECT elems * iters FROM params)
   OR bytes  != (SELECT elems * iters * elem_bytes FROM params)
ORDER BY rank;
