-- requires: nccl_comm nccl_group params.ranks
-- One communicator opened and CLOSED per rank, and every group flat.
--
-- The close half is what entity_close_accounted checks structurally, and this
-- pins the count. A provider that stopped emitting the EntityClose at
-- ncclCommDestroy would leave that assertion quiet — an unclosed entity is
-- exempt for mem_object and ib_device, and adding nccl_comm to that list would
-- turn a red board green. This makes the silence visible.
--
-- The workload opens one group per iteration and never nests one, so depth is
-- 1 and both edges (Start, End) appear. Replaces the lifecycle golden, which
-- pinned the same four numbers for four ranks.
SELECT 'comms opened' AS problem, (SELECT count(*) FROM nccl_comm) AS n
WHERE (SELECT count(*) FROM nccl_comm) != (SELECT ranks FROM params)

UNION ALL
SELECT 'comms closed', (SELECT count(*) FROM entity_close WHERE CAST(opener AS INTEGER) = 900)
WHERE (SELECT count(*) FROM entity_close WHERE CAST(opener AS INTEGER) = 900)
      != (SELECT ranks FROM params)

UNION ALL
SELECT 'group edge kinds', (SELECT count(DISTINCT edge) FROM nccl_group)
WHERE (SELECT count(DISTINCT edge) FROM nccl_group) != 2

UNION ALL
SELECT 'nested group', (SELECT max(CAST(depth AS INTEGER)) FROM nccl_group)
WHERE (SELECT max(CAST(depth AS INTEGER)) FROM nccl_group) != 1;
