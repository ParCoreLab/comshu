-- requires: mem_object gpu_access
-- A rank's memory objects live on GPUs that rank runs on.
--
-- An object is memory a rank ALLOCATED. A rank allocates on the GPUs it drives,
-- so every device an object of rank r names must be one rank r issued GPU work
-- from. A rank holding an object on a GPU it never ran on is holding a WINDOW
-- onto another process's memory and calling it its own.
--
-- That is the regression this exists for. The engine's cuMemMap door recorded
-- every mapping as an object of the mapper, including NCCL's mappings of each
-- peer's buffer (an imported handle): rank 0 then "owned" buffers on GPUs 1 and
-- 3, the reader's device-owner view credited GPU 1's memory to rank 0, and
-- 589,935 of nccl_p2p's peer writes were placed in the WRITER's object (MN5
-- 49207744 through 49220686). Every other check passed, because each offset did
-- fit inside the wrong buffer.
--
-- Run devices come from gpu_access (where the SM ran), so this runs only on
-- captures with GPU accesses, and only for ranks that issued any: a rank with
-- no run-device evidence is not judged. Names no rank or device.
WITH runs AS (
    SELECT DISTINCT rank, CAST(run_dev AS INTEGER) AS device FROM gpu_access
),
judged AS (SELECT DISTINCT rank FROM runs)
SELECT m.rank, CAST(m.device AS INTEGER) AS device, count(*) AS objects,
       group_concat(DISTINCT m.origin) AS origins
FROM mem_object m
JOIN judged j ON j.rank = m.rank
LEFT JOIN runs r ON r.rank = m.rank AND r.device = CAST(m.device AS INTEGER)
WHERE CAST(m.device AS INTEGER) <> 65535
  AND r.device IS NULL
GROUP BY m.rank, CAST(m.device AS INTEGER)
ORDER BY m.rank, device;
