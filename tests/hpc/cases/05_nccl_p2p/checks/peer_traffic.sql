-- The signal only the p2p/gpumem provider can see: GPU-initiated access to
-- ANOTHER device's memory over NVLink (run_dev != mem_dev). Non-empty here,
-- empty in the pure-IB case 02.
--
-- Asserted as a per-rank boolean rather than as pairs or counts. The exact
-- (run_dev, mem_dev) set is not stable run to run — two identical runs differed
-- (3,0) vs (2,8) even with a >1000-access floor — and NVSHMEM/NCCL device
-- attribution is independently known to be approximate (see
-- things-to-circle-back-to.md). The invariant that survives is that every rank
-- does peer traffic at all.
SELECT rank,
       CASE WHEN sum(CASE WHEN run_dev != mem_dev THEN 1 ELSE 0 END) > 0
            THEN 1 ELSE 0 END AS has_peer_traffic
FROM gpu_access
GROUP BY rank
ORDER BY CAST(rank AS INTEGER);
