-- requires: gpu_access
-- The star must be a star: every peer access either goes TO the hub or FROM it,
-- and no spoke ever talks to another spoke.
--
-- The hub is device 0 by construction (the workload makes PE 0 the hub, and on
-- one node PE i runs on device i). This is not read back off the capture — if
-- it were, the assertion would agree with whatever shape was recorded, which is
-- what makes a golden useless for questions of shape.
--
-- gpu_access is peer-only by construction (gpumem's RemoteFilter drops
-- run_dev == mem_dev), so every row here is genuine communication.
--
-- CAST because sql_check.sh loads these CSVs into sqlite, where every column is
-- TEXT and "10" sorts below "9".
SELECT 'spoke-to-spoke access in a star topology' AS problem,
       run_dev, mem_dev, count(*) AS accesses
FROM gpu_access
WHERE CAST(run_dev AS INTEGER) != 0
  AND CAST(mem_dev AS INTEGER) != 0
GROUP BY run_dev, mem_dev
ORDER BY CAST(run_dev AS INTEGER), CAST(mem_dev AS INTEGER);
