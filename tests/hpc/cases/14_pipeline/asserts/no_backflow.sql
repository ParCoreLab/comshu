-- requires: gpu_access
-- No ordered pair may appear in both directions.
--
-- THE POINT OF THE WHOLE CASE. Until this workload existed every capture was a
-- ring or a pair, so A->B and B->A were both always present in equal volume,
-- and an aggregate that silently symmetrised an edge would have produced
-- exactly the right answer on every fixture we had. This is the assertion that
-- distinguishes "direction is recorded" from "direction is assumed".
--
-- It is scoped to device pairs rather than ranks because gpu_access is where
-- direction originates; a rank-space version would also test the join.
SELECT 'peer access recorded in both directions on a one-way chain' AS problem,
       a.run_dev AS from_dev, a.mem_dev AS to_dev,
       count(*) AS accesses
FROM gpu_access a
WHERE EXISTS (SELECT 1 FROM gpu_access b
               WHERE CAST(b.run_dev AS INTEGER) = CAST(a.mem_dev AS INTEGER)
                 AND CAST(b.mem_dev AS INTEGER) = CAST(a.run_dev AS INTEGER))
GROUP BY a.run_dev, a.mem_dev
ORDER BY CAST(a.run_dev AS INTEGER);
