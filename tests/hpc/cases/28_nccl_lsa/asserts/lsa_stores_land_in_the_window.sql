-- requires: object_access params.ranks params.iters params.window_bytes
-- Every window store lands in the RIGHT NEIGHBOUR's window, at the offset the
-- kernel named, and nothing is left unplaced.
--
-- This is the case's claim. ncclGetLsaPointer(w, off, r) is lsaFlatBase +
-- r*stride + off: the writer's address lies in its own flat mapping of the
-- owner's allocation, which is an IMPORT and so not the writer's object. The
-- byte is placeable only if the capture translates it back to the owner's own
-- address for it — what owner_address does for NVSHMEM's peer heap.
--
-- Derived, per actor rank r: owner (r+1) % ranks, window_bytes * iters bytes,
-- offsets exactly [0, window_bytes) — the window is registered at the start
-- of its ncclMemAlloc allocation — and every row exact. One object per owner,
-- so a duplicate record of the same memory (NCCL also maps the owner's OWN
-- allocation into the flat range) cannot split the traffic.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
),
placed AS (
  SELECT actor_rank, owner_rank,
         count(DISTINCT object)              AS objects,
         sum(size)                           AS bytes,
         min(obj_offset)                     AS lo,
         max(obj_offset + size)              AS hi,
         count(*) FILTER (WHERE NOT exact)   AS inexact
  FROM object_access
  WHERE source = 'p2p'
  GROUP BY actor_rank, owner_rank
)
SELECT e.r AS actor_rank, p.owner_rank, p.objects,
       coalesce(p.bytes, 0) AS bytes_placed,
       (SELECT window_bytes * iters FROM params) AS expected,
       p.lo, p.hi, p.inexact
FROM expected e
LEFT JOIN placed p ON p.actor_rank = e.r
WHERE coalesce(p.bytes, 0) != (SELECT window_bytes * iters FROM params)
   OR p.owner_rank != (e.r + 1) % (SELECT ranks FROM params)
   OR p.objects != 1
   OR p.lo != 0
   OR p.hi != (SELECT window_bytes FROM params)
   OR p.inexact > 0;
