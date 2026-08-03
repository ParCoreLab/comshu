-- requires: gpu_access params.ranks params.iters params.elems params.elem_bytes
-- `putget 65536 3`: each PE does `elems` nvshmem_int_p to its neighbour's
-- symmetric heap, `iters` times, so every rank issues elems*iters = 196608
-- peer stores of 4 B. The oracle is the command line.
--
-- Stores only, and gpu_access is peer-only by construction — engine.hpp's
-- RemoteFilter keeps an access iff run_device != mem_device — so this counts
-- exactly the ring's NVLink hops and nothing else. (The kernel's other memory
-- reference, `src[i]`, is a local load and must not appear at all; that is
-- no_local_loads.sql, which is where the attribution bug lands.)
--
-- This is the volume oracle for the case, and it holds even while the device
-- attribution of those accesses does not. Worth being precise about what that
-- means: we capture the right accesses and the right number of them, and
-- mislabel which GPU owns the memory.
--
-- Driven off the workload's rank list so a rank that captured nothing is an
-- offending row rather than a group that never forms.
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
