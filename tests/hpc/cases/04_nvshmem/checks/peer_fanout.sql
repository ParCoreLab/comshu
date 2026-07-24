-- How many distinct peers each GPU writes to (fan-out), not *which* — the ring
-- put has each PE store to one neighbor's symmetric heap. NB: NVSHMEM 3.7's
-- fragmented heap breaks positional device attribution, so the mem_device *ids*
-- are unreliable and run-varying; the fan-out count is the stable invariant. The
-- real volume oracle is traffic.sql.
SELECT run_dev, count(DISTINCT mem_dev) AS peers
FROM gpu_access
WHERE run_dev != mem_dev
GROUP BY run_dev
ORDER BY run_dev;
