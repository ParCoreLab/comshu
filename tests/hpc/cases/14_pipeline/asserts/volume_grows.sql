-- requires: gpu_access
-- Volume increases along the chain: stage i sends slot*(i+1)/npes, so each
-- link carries strictly more than the one before it.
--
-- Checked as a monotonicity property rather than as three expected numbers,
-- because the numbers are the workload's launch geometry and the ORDER is what
-- says the per-edge volumes were attributed to the right edges. A capture that
-- got the edge set right and the volumes swapped passes chain_only and fails
-- this.
SELECT 'chain volume does not increase from one stage to the next' AS problem,
       lo.run_dev AS stage, lo.bytes AS bytes_here, hi.bytes AS bytes_next
FROM (SELECT run_dev, sum(CAST(size AS INTEGER)) AS bytes
        FROM gpu_access GROUP BY run_dev) lo
JOIN (SELECT run_dev, sum(CAST(size AS INTEGER)) AS bytes
        FROM gpu_access GROUP BY run_dev) hi
  ON CAST(hi.run_dev AS INTEGER) = CAST(lo.run_dev AS INTEGER) + 1
WHERE hi.bytes <= lo.bytes
ORDER BY CAST(lo.run_dev AS INTEGER);
