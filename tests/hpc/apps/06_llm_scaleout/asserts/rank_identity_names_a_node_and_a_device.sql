-- requires: rank_identity params.ranks params.nodes params.ranks_per_node
-- Sixteen ranks, four nodes, four ranks each, and every rank naming its GPU.
--
-- THE DEVICE HALF IS A RETEST OF A FILED DEFECT. 04_llmc round 2 measured
-- rank_identity.device NULL on every rank of every capture -- including a
-- `-p nccl` capture whose own nccl_comm.cuda_dev named the device three columns
-- away -- with label `n0 . GPU ?`, while link_health reported all eighteen of its
-- findings at n = 0. A consumer joining on that column gets no rows and no
-- error. Since then "rank_identity.device now resolves on UCX-only captures";
-- this capture has no UCX in it at all, so clause 4 is the check on whether the
-- fix reached the NCCL/verbs lane or only the UCX one.
--
-- THE NODE HALF HAS NEVER BEEN CHECKED AT FOUR NODES. Two nodes make
-- "partitioned correctly" and "partitioned by parity" and several other wrong
-- answers indistinguishable. Four nodes of four do not: clause 3 states the
-- partition as a biconditional over every pair of ranks -- two ranks share a
-- node_hash IF AND ONLY IF `--map-by ppr:4:node` put them on one -- which pins
-- the whole grouping without naming a single hash value the run produced.
--
-- CLAUSE 5 IS THE ONE THAT A CONSTANT WOULD SURVIVE CLAUSE 4 BUT NOT THIS.
-- zero.cuh:448 is `cudaSetDevice(local_device_idx)`, so the four ranks sharing a
-- node drive four DIFFERENT GPUs. A `device` column that resolved to 0
-- everywhere is non-null and useless, and only this clause says so.
--
-- CLAUSE 6 IS THE EXACT FORM. Under `--map-by ppr:4:node` the node-local index
-- is rank % ranks_per_node, which is also what nccl_comm.cuda_dev must hold --
-- so this and the_two_layers_share_one_rank_space.sql together demand that two
-- independently produced columns agree on the same sixteen numbers.
SELECT 'rank_identity does not hold one row per rank' AS problem,
       (SELECT count(*) FROM rank_identity), (SELECT ranks FROM params), '-'
WHERE (SELECT count(*) FROM rank_identity) <> (SELECT ranks FROM params)

UNION ALL

SELECT 'the wrong number of distinct nodes',
       (SELECT count(DISTINCT node_hash) FROM rank_identity), (SELECT nodes FROM params), '-'
WHERE (SELECT count(DISTINCT node_hash) FROM rank_identity) <> (SELECT nodes FROM params)

UNION ALL

SELECT 'two ranks share a node in the capture but not under the launcher, or the reverse',
       a.rank, b.rank, CAST(a.node_hash = b.node_hash AS VARCHAR)
FROM rank_identity a JOIN rank_identity b ON b.rank > a.rank
WHERE (a.node_hash = b.node_hash)
   <> (a.rank // (SELECT ranks_per_node FROM params)
     = b.rank // (SELECT ranks_per_node FROM params))

UNION ALL

SELECT 'rank_identity names no device for a rank',
       rank, -1, COALESCE(CAST(label AS VARCHAR), '<null>')
FROM rank_identity WHERE device IS NULL

UNION ALL

SELECT 'the four ranks of a node do not name four distinct devices',
       count(*), count(DISTINCT device), CAST(node_hash AS VARCHAR)
FROM rank_identity WHERE device IS NOT NULL
GROUP BY node_hash HAVING count(DISTINCT device) <> count(*)

UNION ALL

SELECT 'rank_identity.device is not the node-local rank',
       rank, CAST(device AS BIGINT),
       CAST(rank % (SELECT ranks_per_node FROM params) AS VARCHAR)
FROM rank_identity
WHERE device IS NOT NULL
  AND CAST(device AS BIGINT) <> rank % (SELECT ranks_per_node FROM params);
