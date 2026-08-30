-- requires: transport_mix params.npp params.ranks_per_node
-- Every rank in this decomposition has at least one partner on its own node, so
-- every rank must show shared-memory traffic.
--
-- THE ORACLE. From the partner table in PREDICTION.md P1.4: rank 0's same-node
-- partner is 1, rank 3's is 2, ranks 1 and 2 have two each, ranks 4 and 6 have
-- 5 and the PME rank, rank 5 has all three, and the PME rank has 4, 5 and 6. Not
-- one rank is fabric-only. This is again a property of cutting a 1-D
-- decomposition across two nodes rather than anything GROMACS decided.
--
-- WHY IT IS WORTH A SEPARATE FILE from the fabric-share checks. Those two ask
-- whether the intra/inter split is in the right PROPORTION. This one asks
-- whether the shared-memory side exists at all, and it is the failure most
-- likely to happen quietly: ucx_deep patches uct_iface_ops_t per interface, and
-- an implementation that reached the InfiniBand interfaces but not the
-- shared-memory ones would produce a capture that looks entirely healthy --
-- non-empty uct_send, sensible byte counts, a clean link_health -- and is
-- silently missing half of every multi-rank-per-node run on the machine.
--
-- Every UCX-run application on MN5 puts four ranks on a node. If this fires,
-- the layer is blind to a majority of the traffic of a majority of the jobs.
--
-- The suite could not have found this. Cases 19, 21 and 22 all run ONE RANK PER
-- NODE, and say so in their meta.json for a good reason -- they wanted the
-- fabric, and a same-node pair would have taken shared memory and never reached
-- rc_mlx5. The consequence is that no workload this project has ever written
-- produces intra-node UCX traffic at all. GROMACS on 4 ranks per node is the
-- first thing to ask the question.
WITH RECURSIVE r(rk) AS (
  SELECT 0
  UNION ALL
  SELECT rk + 1 FROM r WHERE rk + 1 < (SELECT ranks FROM params)
),
obs AS (
  SELECT CAST(rank AS INTEGER) AS rk,
         sum(CASE WHEN tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                                   'cuda_ipc','cuda_copy','gdr_copy')
                  THEN CAST(ops AS BIGINT) ELSE 0 END) AS shm_ops,
         sum(CASE WHEN tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                                   'cuda_ipc','cuda_copy','gdr_copy')
                  THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END) AS shm_bytes
  FROM transport_mix
  GROUP BY CAST(rank AS INTEGER)
)
SELECT 'a rank recorded no shared-memory operation, though the decomposition gives it a same-node partner' AS problem,
       r.rk AS rank,
       COALESCE((SELECT shm_ops   FROM obs o WHERE o.rk = r.rk), 0) AS shm_ops,
       COALESCE((SELECT shm_bytes FROM obs o WHERE o.rk = r.rk), 0) AS shm_bytes
FROM r
WHERE COALESCE((SELECT shm_ops FROM obs o WHERE o.rk = r.rk), 0) = 0

UNION ALL

-- The whole-capture form of the same question, which fires with one row rather
-- than eight and names the cause a reader should check first.
SELECT 'no shared-memory transport appears anywhere in transport_mix',
       -1,
       (SELECT count(*) FROM transport_mix),
       (SELECT count(DISTINCT tl_name) FROM transport_mix)
WHERE NOT EXISTS (
  SELECT 1 FROM transport_mix
  WHERE tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                    'cuda_ipc','cuda_copy','gdr_copy')
);
