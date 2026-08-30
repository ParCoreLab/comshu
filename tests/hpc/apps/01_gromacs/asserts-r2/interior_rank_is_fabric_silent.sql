-- requires: transport_mix params.npp params.ranks_per_node params.interior_fabric_max_pct
-- A rank whose every communication partner is on its own node must move no
-- fabric bytes. A rank that has a partner off-node must move some.
--
-- THIS IS THE SHARPEST CLAIM IN THE STUDY and it costs nothing to derive. With
-- `-npme 1 -dd 7 1 1` over 2 nodes of 4 ranks, PP rank x has partners
-- (x+-1) mod 7 and the PME rank 7, and ranks 0-3 sit on one node with 4-7 on the
-- other. Work that through and exactly one rank has all three partners on its
-- own node: the interior cell of the second node's slab. Its halo neighbours and
-- its PME rank are all beside it. Every byte it sends must go over shared memory
-- and none over InfiniBand.
--
-- Nothing about that rank is special to GROMACS or to UCX -- it is a
-- consequence of a 1-D decomposition being cut across two nodes, which is what
-- every multi-node MD run in the world looks like. That is what makes it an
-- oracle rather than an observation.
--
-- IT NAMES NO RANK. The interior rank is COMPUTED below from npp and
-- ranks_per_node, so this file contains no rank number and would still be
-- correct at a different rank count or a different node width. Writing `5` here
-- would be a golden wearing a costume, by the suite's own definition.
--
-- WHAT IT CATCHES, and it is the failure comm_edges made structurally
-- impossible to notice: per-rank misattribution. A transport_mix that assigned
-- each interface's traffic to the wrong rank, or that classified every UCT
-- interface the same way, would still produce a plausible aggregate -- the
-- headline fraction in fabric_share_matches_the_decomposition.sql would still
-- land in band -- while being wrong about every individual rank. Only a
-- per-rank oracle sees that, and only a real decomposition provides one.
--
-- THE SECOND CLAUSE IS THE CONTROL. If transport_mix simply reported no fabric
-- traffic for anybody, the first clause would pass vacuously and this study
-- would report a triumph. So the same query also demands that every rank the
-- decomposition gives an off-node partner has a fabric share above a floor.
WITH RECURSIVE r(rk) AS (
  SELECT 0
  UNION ALL
  SELECT rk + 1 FROM r WHERE rk + 1 < (SELECT ranks FROM params)
),
-- Every ordered partner pair the decomposition implies: the halo 7-cycle among
-- the PP ranks, and the PP <-> PME star in both directions.
peer AS (
  SELECT rk AS a, (rk + 1) % (SELECT npp FROM params) AS b
  FROM r WHERE rk < (SELECT npp FROM params)
  UNION
  SELECT rk, (rk + (SELECT npp FROM params) - 1) % (SELECT npp FROM params)
  FROM r WHERE rk < (SELECT npp FROM params)
  UNION
  SELECT rk, (SELECT npp FROM params)
  FROM r WHERE rk < (SELECT npp FROM params)
  UNION
  SELECT (SELECT npp FROM params), rk
  FROM r WHERE rk < (SELECT npp FROM params)
),
-- node(x) written as (x - x % k) / k rather than x / k, because integer division
-- is `/` in sqlite and `//` in DuckDB and this form is exact in both.
crossing AS (
  SELECT a AS rk,
         sum(CASE WHEN (a - a % (SELECT ranks_per_node FROM params))
                    <> (b - b % (SELECT ranks_per_node FROM params))
                  THEN 1 ELSE 0 END) AS offnode_partners
  FROM peer
  GROUP BY a
),
obs AS (
  SELECT CAST(rank AS INTEGER) AS rk,
         sum(CASE WHEN tl_name IN ('rc_mlx5','rc_verbs','dc_mlx5','ud_mlx5','ud_verbs')
                  THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END) AS fab,
         sum(CASE WHEN tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                                   'cuda_ipc','cuda_copy','gdr_copy')
                  THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END) AS shm
  FROM transport_mix
  GROUP BY CAST(rank AS INTEGER)
)
SELECT 'a rank whose every partner is on its own node still moved fabric bytes' AS problem,
       o.rk AS rank, c.offnode_partners, o.fab AS fabric_bytes, o.shm AS shm_bytes,
       (100 * o.fab) / (o.fab + o.shm) AS fabric_pct
FROM obs o JOIN crossing c ON c.rk = o.rk
WHERE c.offnode_partners = 0
  AND o.fab + o.shm > 0
  AND (100 * o.fab) / (o.fab + o.shm) >= (SELECT interior_fabric_max_pct FROM params)

UNION ALL

SELECT 'a rank the decomposition gives an off-node partner moved almost no fabric bytes',
       o.rk, c.offnode_partners, o.fab, o.shm,
       (100 * o.fab) / (o.fab + o.shm)
FROM obs o JOIN crossing c ON c.rk = o.rk
WHERE c.offnode_partners > 0
  AND o.fab + o.shm > 0
  AND (100 * o.fab) / (o.fab + o.shm) <= (SELECT exterior_fabric_min_pct FROM params)

UNION ALL

-- A rank the decomposition knows about that transport_mix has no row for at
-- all. Distinct from the two above, and a different defect: not a
-- misclassification but a missing rank.
SELECT 'a rank of the decomposition has no transport_mix row',
       c.rk, c.offnode_partners, -1, -1, -1
FROM crossing c
WHERE NOT EXISTS (SELECT 1 FROM obs o WHERE o.rk = c.rk);
