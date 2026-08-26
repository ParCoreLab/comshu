-- requires: nccl_comm params.ranks params.gpus
-- llm.c builds exactly one communicator, over every process, and the capture's
-- cross-rank join finds it.
--
-- multi_gpu_config_init (zero.cuh:411-465) calls ncclCommInitRank ONCE
-- (zero.cuh:449) with num_processes and the single ncclUniqueId that rank 0
-- generated and MPI_Bcast broadcast. There is no ncclCommSplit and no second
-- communicator anywhere in llm.c, so `nranks` is the launcher's process count
-- and every member must agree on the id.
--
-- comm_hash is FNV-1a over that broadcast id, so it is equal across the group by
-- construction. This is the assertion the record type exists for and the only
-- one that cannot be checked on a single rank: ncclComm_t is a process-local
-- pointer, so without the hash nothing in a capture connects rank 0's
-- communicator to rank 7's.
--
-- On MN5 this runs at 8 ranks across 2 nodes, which is where it earns its keep —
-- the id crosses the node boundary through MPI before NCCL ever sees it, so a
-- hash read at the wrong moment (after NCCL has consumed the id) would agree
-- within a node and disagree across one.
--
-- Names no hash and no rank; the only constant is params.ranks.
SELECT * FROM (
  SELECT 'not every rank recorded a communicator' AS problem, NULL AS detail
  WHERE (SELECT count(DISTINCT rank) FROM nccl_comm) != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'rank recorded more than one communicator — llm.c builds exactly one',
         CAST(rank AS VARCHAR)
  FROM nccl_comm GROUP BY rank HAVING count(*) != 1

  UNION ALL
  SELECT 'ranks disagree on comm_hash — the cross-rank join is broken',
         CAST((SELECT count(DISTINCT comm_hash) FROM nccl_comm) AS VARCHAR)
  WHERE (SELECT count(DISTINCT comm_hash) FROM nccl_comm) != 1

  UNION ALL
  SELECT 'comm_hash is zero', CAST(rank AS VARCHAR)
  FROM nccl_comm WHERE comm_hash = 0

  UNION ALL
  SELECT 'nranks disagrees with the launcher', CAST(nranks AS VARCHAR)
  FROM nccl_comm WHERE nranks != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'NCCL ranks are not distinct across the communicator',
         CAST((SELECT count(DISTINCT nccl_rank) FROM nccl_comm) AS VARCHAR)
  WHERE (SELECT count(DISTINCT nccl_rank) FROM nccl_comm) != (SELECT ranks FROM params)

  UNION ALL
  -- One process per GPU: llm.c sets local_device_idx = process_rank % gpus_per_node
  -- (zero.cuh:433) or derives it from the hostname hash under MPI, either way one
  -- device per rank. Two ranks on one node holding the same cuda_dev would mean
  -- the capture's device attribution is wrong, or the launcher oversubscribed.
  SELECT 'a cuda device is claimed by more than one rank on this node',
         CAST(cuda_dev AS VARCHAR)
  FROM nccl_comm GROUP BY cuda_dev
  HAVING count(*) != (SELECT ranks / gpus FROM params)
)
ORDER BY problem, detail;
