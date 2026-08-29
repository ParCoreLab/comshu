-- requires: gpu_kernel nccl_comm params.ranks params.gpus params.nodes
-- The two providers must agree about which GPU a rank is.
--
-- ROUND 2, and it is the cheapest cross-provider check there is. `nccl_comm`
-- records `cuda_dev` from the communicator; `gpu_kernel` records `device` from
-- the CUPTI activity record. They are produced by different instruments in the
-- same process, and llm.c gives them one right answer: multi_gpu_config_init
-- calls cudaSetDevice(local_device_idx) once (zero.cuh:448) and never changes
-- device again, so every kernel of a rank runs on that rank's one GPU.
--
-- WHAT IT CATCHES:
--   * a rank whose kernels name two devices — either the device field is being
--     read per-context instead of per-launch, or two ranks' streams were merged
--     into one rank directory;
--   * kernel device != communicator device — the two providers' device spaces
--     disagree, which would silently break any join between them;
--   * a device ordinal outside the node's GPU count — the `owner_in_range`
--     failure that found NVSHMEM's phantom devices in case 04;
--   * two ranks on one node claiming one GPU — an oversubscribed launcher or a
--     rank-attribution error in the merge.
--
-- Names no rank and no device: `ranks`, `gpus` and `nodes` come from params,
-- and every comparison is between two columns of the capture.
SELECT * FROM (
  SELECT 'not every rank recorded kernels' AS problem, NULL AS rank, NULL AS detail
  WHERE (SELECT count(DISTINCT rank) FROM gpu_kernel) != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'a rank ran kernels on more than one device',
         CAST(rank AS INTEGER), CAST(count(DISTINCT device) AS VARCHAR)
  FROM gpu_kernel GROUP BY rank HAVING count(DISTINCT device) != 1

  UNION ALL
  SELECT 'kernel device disagrees with the communicator device',
         CAST(c.rank AS INTEGER),
         CAST(c.cuda_dev AS VARCHAR) || ' vs ' || CAST(k.dev AS VARCHAR)
  FROM nccl_comm c
  JOIN (SELECT rank, min(device) AS dev FROM gpu_kernel GROUP BY rank) k
    ON k.rank = c.rank
  WHERE CAST(c.cuda_dev AS INTEGER) != CAST(k.dev AS INTEGER)

  UNION ALL
  SELECT 'kernel ran on a device this node does not have',
         CAST(rank AS INTEGER), CAST(device AS VARCHAR)
  FROM gpu_kernel
  WHERE CAST(device AS INTEGER) < 0
     OR CAST(device AS INTEGER) >= (SELECT gpus FROM params)
  GROUP BY rank, device

  UNION ALL
  SELECT 'one GPU claimed by more ranks than the topology allows',
         NULL, CAST(device AS VARCHAR)
  FROM (SELECT DISTINCT rank, device FROM gpu_kernel)
  GROUP BY device
  HAVING count(*) != (SELECT ranks / (gpus * nodes) FROM params)
)
ORDER BY problem, rank;
