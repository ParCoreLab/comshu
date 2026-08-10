-- requires: gpu_access params.ranks
-- The hub's receive buffer must be ONE object written by EVERY spoke.
--
-- This is the case's reason to exist. Every other capture in the corpus has one
-- writer per object, so "which GPUs touched this buffer" was a column that could
-- never be wrong — every answer was a single GPU and a bug would have looked
-- identical. Here the hub's recv is written by ranks-1 distinct devices into
-- disjoint regions (dst_off = mype*slot), so the object view's `touched by` and
-- its banded heatmap have something to be wrong about.
--
-- Stated as "fewer writers than expected": more would mean the workload changed,
-- fewer means attribution collapsed several writers into one, which is the
-- failure mode worth catching.
SELECT 'hub buffer not written by every spoke' AS problem,
       object,
       count(DISTINCT run_dev) AS writers,
       (SELECT ranks FROM params) - 1 AS expected_writers
FROM gpu_access
WHERE CAST(mem_dev AS INTEGER) = 0
GROUP BY object
HAVING count(DISTINCT run_dev) < (SELECT ranks FROM params) - 1;
