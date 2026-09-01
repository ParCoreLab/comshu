-- requires: rank_identity params.ranks params.nodes params.ranks_per_node params.placement
-- rank_identity must place 16 ranks on 4 nodes in exactly the grouping mpirun
-- used, and it must name a GPU for each of them.
--
-- WHY IT MATTERS BEYOND ITS OWN ROW COUNT. comm_edges.scope is derived from node
-- membership, so scope_follows_the_launcher_placement.sql is downstream of this
-- table being right. If rank_identity groups the ranks wrongly then that
-- assertion could fail for a reason that has nothing to do with `scope` itself,
-- and this file is what separates the two.
--
-- THE ORACLE IS AGAIN THE LAUNCHER. `--map-by ppr:4:node` groups ranks
-- {0..3},{4..7},... ; `--map-by node` groups {0,4,8,12},{1,5,9,13},... The job
-- script prints hostname per rank from the BARE run so the grouping is
-- established by the operating system before any capture exists. Clause 2
-- states it as "two ranks share a node_hash if and only if the placement rule
-- says they do", which pins the partition without naming a hash value.
--
-- THE DEVICE HALF IS A RETEST OF A KNOWN DEFECT. 04_llmc round 2 measured
-- rank_identity.device NULL on every rank of every capture, with label
-- `n0 . GPU ?`, while nccl_comm.cuda_dev in the same export named the device --
-- and link_health reported the capture healthy. Since then "rank_identity.device
-- now resolves on UCX-only captures", and this is a UCX-only capture, so
-- clause 3 is the check on that claim. Clause 4 is the stronger form: the four
-- ranks sharing a node must name four DISTINCT devices, because LAMMPS's
-- `-k on g 4` gives each local rank its own GPU, so a column that resolved to a
-- constant would pass clause 3 and fail clause 4.
SELECT 'rank_identity does not hold one row per rank' AS problem,
       (SELECT count(*) FROM rank_identity), (SELECT ranks FROM params), '-', '-'
WHERE (SELECT count(*) FROM rank_identity) <> (SELECT ranks FROM params)

UNION ALL

SELECT 'the wrong number of distinct nodes',
       (SELECT count(DISTINCT node_hash) FROM rank_identity), (SELECT nodes FROM params), '-', '-'
WHERE (SELECT count(DISTINCT node_hash) FROM rank_identity) <> (SELECT nodes FROM params)

UNION ALL

SELECT 'two ranks share a node in the capture but not under the launcher, or the reverse',
       a.rank, b.rank, CAST(a.node_hash = b.node_hash AS VARCHAR),
       CAST(CASE WHEN (SELECT placement FROM params) = 0
                 THEN a.rank / (SELECT ranks_per_node FROM params) = b.rank / (SELECT ranks_per_node FROM params)
                 ELSE a.rank % (SELECT nodes FROM params) = b.rank % (SELECT nodes FROM params) END AS VARCHAR)
FROM rank_identity a JOIN rank_identity b ON b.rank > a.rank
WHERE (a.node_hash = b.node_hash)
   <> (CASE WHEN (SELECT placement FROM params) = 0
            THEN a.rank / (SELECT ranks_per_node FROM params) = b.rank / (SELECT ranks_per_node FROM params)
            ELSE a.rank % (SELECT nodes FROM params) = b.rank % (SELECT nodes FROM params) END)

UNION ALL

SELECT 'rank_identity names no device for a rank', rank, -1,
       COALESCE(CAST(device AS VARCHAR), '<null>'), COALESCE(CAST(label AS VARCHAR), '<null>')
FROM rank_identity WHERE device IS NULL

UNION ALL

SELECT 'the ranks sharing a node do not name distinct devices',
       count(*), count(DISTINCT device), CAST(node_hash AS VARCHAR), '-'
FROM rank_identity WHERE device IS NOT NULL
GROUP BY node_hash HAVING count(DISTINCT device) <> count(*);
