-- requires: gpu_access params.ranks params.gpus params.iters params.elems params.elem_bytes
-- `p2p_kernel 4096 4` on 4 GPUs: each GPU stores `elems` ints to its ring
-- neighbour, `iters` times, and every GPU runs the loop — so the single process
-- records gpus*iters*elems = 65536 stores of 4 B. One process drives all four
-- devices (COMSHU_RANK=0), so the total is per rank.
--
-- Stores only. peer_rw also reads `local[i]`, but that read stays on the GPU
-- that issues it and gpu_access is peer-only by construction (engine.hpp's
-- RemoteFilter keeps an access iff run_device != mem_device), so a `load` row
-- here would mean a local access had been attributed to the wrong device — the
-- 04_nvshmem failure, arriving on the case where attribution is known good.
-- That is the second branch, and it is separate because a store count alone
-- cannot notice extra rows of a different op.
--
-- This is the case where every quantity is computable, which is what makes it
-- the control for the ones where they are not.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT 'wrong store volume' AS problem, r AS rank,
         (SELECT count(*) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store') AS n,
         (SELECT coalesce(sum(CAST(g.size AS INTEGER)), 0) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store') AS bytes
  FROM expected
  WHERE (SELECT count(*) FROM gpu_access g
         WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store')
        != (SELECT gpus * iters * elems FROM params)
     OR (SELECT coalesce(sum(CAST(g.size AS INTEGER)), 0) FROM gpu_access g
         WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store')
        != (SELECT gpus * iters * elems * elem_bytes FROM params)

  UNION ALL
  SELECT 'unexpected op: ' || op, CAST(rank AS INTEGER), count(*), NULL
  FROM gpu_access WHERE op <> 'store'
  GROUP BY rank, op
)
ORDER BY problem, rank;
