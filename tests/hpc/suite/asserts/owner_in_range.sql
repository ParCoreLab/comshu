-- requires: gpu_access params.gpus
-- Every device ordinal a GPU access is attributed to must name a GPU that
-- exists. `mem_dev` is the device that OWNS the memory being touched, so it
-- indexes the node's GPUs and must be in [0, gpus).
--
-- Trivially true for a correct capture, which is exactly why it is worth
-- running everywhere. On 07_nvshmem_multinode job 44111484 the four devices
-- came back as 64,65,66,67; on 04_nvshmem they are 65..69 in one run and
-- 238..242 in another. That corruption surfaced three weeks late as a doubled
-- access count nobody could explain, because no check ever asked whether the
-- owner id was a device at all. This one asks, on every case that has
-- gpu_access, and costs one scan.
--
-- Names no device: the bound comes from the workload's GPU count.
SELECT run_dev,
       mem_dev,
       count(*) AS accesses
FROM gpu_access
WHERE CAST(mem_dev AS INTEGER) <  0
   OR CAST(mem_dev AS INTEGER) >= (SELECT gpus FROM params)
   OR CAST(run_dev AS INTEGER) <  0
   OR CAST(run_dev AS INTEGER) >= (SELECT gpus FROM params)
GROUP BY run_dev, mem_dev
ORDER BY CAST(run_dev AS INTEGER), CAST(mem_dev AS INTEGER);
