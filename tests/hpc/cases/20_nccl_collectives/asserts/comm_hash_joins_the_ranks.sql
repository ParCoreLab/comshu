-- requires: nccl_comm params.ranks
-- The cross-rank join works. This is the assertion the whole record type exists
-- for, and the only one that cannot be checked on a single rank.
--
-- A ncclComm_t is a process-local pointer: rank 0's comm and rank 3's comm are
-- unrelated numbers naming the same communicator, so nothing in a capture could
-- connect them. comm_hash is FNV-1a over the ncclUniqueId, which every member
-- was initialised with, so it is EQUAL across the group by construction — the
-- same trick ucx_worker_address uses, and the same one qp_num could not pull off
-- because it is only unique per HCA.
--
-- If this fires, a comm matrix built over NCCL is joining ranks that are not in
-- the same communicator, or failing to join ranks that are. Three ways it can:
-- the hash is computed over the wrong bytes, the id is read after NCCL has
-- consumed it, or a rank recorded no communicator at all.
--
-- Names no hash and no rank. The rank count comes from params; the hash is only
-- ever compared with itself.
SELECT * FROM (
  SELECT 'not every rank recorded a communicator' AS problem, NULL AS detail
  WHERE (SELECT count(DISTINCT rank) FROM nccl_comm) != (SELECT ranks FROM params)

  UNION ALL
  -- One communicator per rank in this workload, and each rank joins it once.
  SELECT 'rank recorded more than one communicator', CAST(rank AS VARCHAR)
  FROM nccl_comm GROUP BY rank HAVING count(*) != 1

  UNION ALL
  -- The join itself: one distinct hash across every rank.
  SELECT 'ranks disagree on comm_hash — the cross-rank join is broken',
         CAST(count(DISTINCT comm_hash) AS VARCHAR)
  FROM nccl_comm
  HAVING count(DISTINCT comm_hash) != 1

  UNION ALL
  -- A zero hash means the id was read as zeroes, which would make every
  -- communicator in every capture join to every other one.
  SELECT 'comm_hash is zero', CAST(rank AS VARCHAR)
  FROM nccl_comm WHERE comm_hash = 0

  UNION ALL
  -- NCCL's own view of the membership must match the workload's, and each rank
  -- must be a distinct member of it. This is what catches a comm recorded from
  -- stale or wrongly-typed arguments.
  SELECT 'nranks disagrees with the workload', CAST(nranks AS VARCHAR)
  FROM nccl_comm WHERE nranks != (SELECT ranks FROM params)

  UNION ALL
  -- `nccl_rank` is the rank INSIDE the communicator and is not the capture's
  -- own rank column — see the reader for why they are named apart. Every member
  -- must hold a distinct one.
  SELECT 'NCCL ranks are not distinct across the communicator',
         CAST(count(DISTINCT nccl_rank) AS VARCHAR)
  FROM nccl_comm
  HAVING count(DISTINCT nccl_rank) != (SELECT ranks FROM params)
)
ORDER BY problem, detail;
