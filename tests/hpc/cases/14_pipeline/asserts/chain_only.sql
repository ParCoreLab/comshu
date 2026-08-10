-- requires: gpu_access
-- Every access must go from device i to device i+1. Nothing else, and in
-- particular nothing wrapping from the last device back to the first — the
-- chain is not a ring, and a ring is what every other NVSHMEM case in this
-- suite is.
--
-- The relation is stated as arithmetic on the two device columns rather than as
-- an enumerated edge list, so it holds at any PE count and cannot be satisfied
-- by a capture that merely resembles the recorded one.
SELECT 'access is not from device i to device i+1' AS problem,
       run_dev, mem_dev, count(*) AS accesses
FROM gpu_access
WHERE CAST(mem_dev AS INTEGER) != CAST(run_dev AS INTEGER) + 1
GROUP BY run_dev, mem_dev
ORDER BY CAST(run_dev AS INTEGER), CAST(mem_dev AS INTEGER);
