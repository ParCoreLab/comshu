-- requires: gpu_access params.gpus params.iters params.elems
-- The kernel is a ring: device d writes to device (d+1) mod gpus and to nothing
-- else, so the access graph is exactly `gpus` edges of iters*elems = 16384
-- accesses each. Every access in this case is a peer access — the kernel never
-- touches its own memory — which is why the edge set is asserted unfiltered.
--
-- Two shapes of offence, because a GROUP BY can only speak about edges that
-- exist: a wrong edge (not the ring's successor, or the wrong size), and the
-- wrong NUMBER of edges, which is how a hop that vanished entirely shows up.
-- The modulus comes from the workload's GPU count; no device ordinal is
-- written down.
--
-- This is the reference for what correct device attribution looks like, and the
-- reason it is worth asserting on the one case where it holds: the identical
-- claim on 04/06/07 is the acceptance test for the NVSHMEM heap-attribution
-- work, where mem_dev comes back as 65..69 or 238..242.
SELECT 'wrong edge' AS problem, run_dev, mem_dev, count(*) AS accesses
FROM gpu_access
GROUP BY run_dev, mem_dev
HAVING CAST(mem_dev AS INTEGER)
         != (CAST(run_dev AS INTEGER) + 1) % (SELECT gpus FROM params)
    OR accesses != (SELECT iters * elems FROM params)

UNION ALL
SELECT 'wrong edge count', NULL, NULL,
       (SELECT count(*) FROM (SELECT DISTINCT run_dev, mem_dev FROM gpu_access))
WHERE (SELECT count(*) FROM (SELECT DISTINCT run_dev, mem_dev FROM gpu_access))
      != (SELECT gpus FROM params)

ORDER BY 1, 2, 3;
