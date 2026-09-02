-- requires: nccl_comm ib_post_send params.ranks params.ranks_per_node params.nodes
-- The NCCL view and the verbs view must be talking about the same sixteen ranks.
--
-- THIS IS THE CROSS-LAYER QUESTION IN ITS MOST BASIC FORM, and it is worth
-- asserting separately from comm_edges because it does not depend on the peer
-- resolution working. `nccl_comm` names every rank of the communicator with its
-- nranks and its comm_hash; `ib_post_send` names every rank that posted on the
-- fabric. If those two rank sets disagree, then any statement of the form "the
-- collective the application asked for became this traffic" is comparing two
-- different populations, and every other assertion in this directory is built
-- on sand.
--
-- WHAT EACH CLAUSE CATCHES:
--   1. A communicator that is not the sixteen-way one llm.c builds. llm.c calls
--      ncclCommInitRank once with the MPI world size (zero.cuh's
--      multi_gpu_config_init), so nranks must be 16 on every rank and there must
--      be exactly one comm_hash across the whole job -- the hash is the only
--      thing in the NCCL records that crosses a rank boundary.
--   2. cuda_dev must be the node-local rank, because zero.cuh:448 is
--      `cudaSetDevice(local_device_idx)` and `--map-by ppr:4:node` makes the
--      local index rank % 4. This is the one field that ties a rank to a
--      physical GPU without going through any derived table.
--   3. A rank that is in the communicator and posted nothing on IB, or the
--      reverse. 04_llmc measured at 8 ranks that ALL ranks post -- NCCL gives
--      every local GPU an inter-node connection rather than routing everything
--      through the one owning a ring hop -- and recorded that as a corrected
--      wrong guess. At 16 ranks over 4 nodes that is a genuinely open question
--      again, so this clause is written to fire either way and the direction of
--      the failure is the measurement.
--
-- NOT VACUOUS: clause 4 refuses an empty nccl_comm, and ib_post_send is required
-- by the header, so both populations must exist for the file to run at all.
SELECT 'the communicator is not the sixteen-way one llm.c builds' AS problem,
       rank, CAST(nranks AS BIGINT), 0, 0
FROM nccl_comm
WHERE CAST(nranks AS BIGINT) <> (SELECT ranks FROM params)

UNION ALL

SELECT 'more than one communicator hash exists across the job',
       -1, (SELECT count(DISTINCT comm_hash) FROM nccl_comm), 0, 0
WHERE (SELECT count(DISTINCT comm_hash) FROM nccl_comm) <> 1

UNION ALL

SELECT 'nccl_comm.cuda_dev is not the node-local rank',
       rank, CAST(cuda_dev AS BIGINT),
       rank % (SELECT ranks_per_node FROM params), 0
FROM nccl_comm
WHERE CAST(cuda_dev AS BIGINT) <> rank % (SELECT ranks_per_node FROM params)

UNION ALL

SELECT 'a rank is in the communicator but posted nothing on the fabric',
       c.rank, 0, 0, 0
FROM nccl_comm c
LEFT JOIN (SELECT DISTINCT rank FROM ib_post_send) s ON s.rank = c.rank
WHERE s.rank IS NULL

UNION ALL

SELECT 'a rank posted on the fabric but is in no communicator',
       s.rank, 0, 0, 0
FROM (SELECT DISTINCT rank FROM ib_post_send) s
LEFT JOIN nccl_comm c ON c.rank = s.rank
WHERE c.rank IS NULL

UNION ALL

SELECT 'nccl_comm is empty, so this assertion checked nothing', -1, 0, 0, 0
WHERE (SELECT count(*) FROM nccl_comm) = 0;
