-- requires: gpu_access params.ranks params.nodes params.elems params.iters
-- The physics of the case, stated without naming a PE.
--
-- putget's ring visits every PE once, so it crosses the node boundary exactly
-- `nodes` times — with 8 PEs over 2 nodes, two hops out of eight. Inter-node
-- NVSHMEM does not move payload with GPU loads and stores into a PGAS window:
-- the device enqueues a descriptor and the NIC performs the transfer. An
-- address tracer therefore sees *nothing* on those hops. So exactly `nodes`
-- ranks record 0 NVLink accesses, and the other ranks - nodes record the full
-- elems*iters = 196608 peer stores.
--
-- Today's golden names the crossers (3 and 7). That is an artefact of
-- --map-by ppr:4:node putting rank 3 last on the first node; a different
-- mapping moves them and the golden goes red for a scheduling decision. The
-- count of crossers is the claim.
--
-- It is also a live correctness check, not just a tidier spelling: in job
-- 44111484 all eight ranks reported 196608 NVLink accesses, crossers included,
-- with mem_dev in 64..67. This query returns "wrong number of crossers, 0" on
-- that capture. The golden returned a diff of sixteen numbers.
SELECT * FROM (
  SELECT 'nonzero rank with wrong volume' AS problem,
         CAST(rank AS INTEGER) AS rank,
         count(*)              AS nvlink_accesses
  FROM gpu_access
  GROUP BY rank
  HAVING nvlink_accesses != (SELECT elems * iters FROM params)

  UNION ALL
  SELECT 'wrong number of crossers', NULL,
         (SELECT ranks FROM params) - (SELECT count(DISTINCT rank) FROM gpu_access)
  WHERE (SELECT ranks FROM params) - (SELECT count(DISTINCT rank) FROM gpu_access)
        != (SELECT nodes FROM params)
)
ORDER BY problem, rank;
