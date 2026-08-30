-- requires: transport_mix params.npool params.nodes
-- THE HEADLINE. Which transport carried the FFT transpose must follow QE's own
-- pool arithmetic, and nothing else changed between the two captures.
--
-- THE ORACLE, and it is four layers deep, none of them a capture:
--   Modules/mp_pools.f90:73,79  nproc_pool = nproc/npool; my_pool_id = mype/nproc_pool
--                               -> pools are CONTIGUOUS BLOCKS of ranks
--   Modules/mp_bands.f90:90     nproc_bgrp = nproc_pool  (nbgrp = 1 here)
--   FFTXlib/src/fft_scatter.f90 the transpose all-to-all runs on sub-communicators
--                               of intra_bgrp_comm, i.e. INSIDE the pool
--   job-r2.sbatch               mpirun --map-by ppr:4:node  -> node(r) = r/(ranks/nodes)
--
-- Compose them: a pool spans a node boundary exactly when there are more ranks
-- in a pool than in a node, i.e. when ranks/npool > ranks/nodes, i.e. when
--
--     npool < nodes
--
-- which is written that way here on purpose -- it needs no division, so it means
-- the same thing on sqlite3 and on DuckDB. With ranks=8, nodes=2: npool=1 spans,
-- npool=2 does not. The SAME FILE therefore makes the OPPOSITE claim in the two
-- arms, driven entirely by params, and names no rank, no node and no transport
-- that the run chose.
--
-- WHY OPERATIONS AND NOT BYTES. pcomm.fbs type 804 says UctSend.payload "means
-- something different per op family -- sent, requested, or advertised -- so it
-- must not be summed across them", and 21_ucx_deep asserts separately that the
-- rendezvous door reports its bytes as advertised_bytes and never as out_bytes.
-- QE's transpose messages are 0.8-3.4 MB, far above any default RNDV_THRESH, so
-- they ARE rendezvous and a byte total would be split across two columns whose
-- sum is meaningless. `ops` is the one additive column. The job script PRINTS
-- the byte breakdown per (scope, tl_name, op) and no assertion reads it.
--
-- WHY tl_name AND NOT scope. `scope` is visibly the column meant for this
-- question, and its value vocabulary is published nowhere I am allowed to read
-- (see QUESTIONS.md). tl_name carries UCX's OWN transport names, which are
-- documented by UCX: rc_mlx5 / dc_mlx5 / ud_mlx5 / rc_verbs / ud_verbs are the
-- InfiniBand ones and none of self / posix / sysv / cma / knem / xpmem /
-- cuda_ipc collides with the pattern. Binding to a third party's vocabulary is
-- weaker than binding to pcomm's and is the best available.
--
-- THE VACUITY GUARD IS NOT OPTIONAL. Without clause 3 the npool=2 arm passes on
-- an empty capture, and "a capture of a workload that did not work is
-- indistinguishable from a tracer that captured nothing" is the confusion
-- apps/README.md says has already cost this project slots.
WITH mix AS (
  SELECT CAST(ops AS INTEGER) AS ops,
         CASE WHEN tl_name LIKE '%mlx5%' OR tl_name LIKE '%verbs%'
              THEN 1 ELSE 0 END AS fabric
  FROM transport_mix
),
t AS (
  SELECT COALESCE(sum(ops), 0)                                     AS all_ops,
         COALESCE(sum(CASE WHEN fabric = 1 THEN ops END), 0)       AS fab_ops,
         COALESCE(sum(CASE WHEN fabric = 0 THEN ops END), 0)       AS local_ops
  FROM mix
)
SELECT 'a pool spans nodes, yet under a quarter of UCT operations took a fabric transport' AS problem,
       (SELECT npool FROM params) AS npool,
       (SELECT nodes FROM params) AS nodes,
       t.fab_ops, t.local_ops, t.all_ops
FROM t
WHERE (SELECT npool FROM params) < (SELECT nodes FROM params)
  AND t.fab_ops * 4 < t.all_ops

UNION ALL

SELECT 'no pool spans a node, yet more than a tenth of UCT operations took a fabric transport',
       (SELECT npool FROM params), (SELECT nodes FROM params),
       t.fab_ops, t.local_ops, t.all_ops
FROM t
WHERE (SELECT npool FROM params) >= (SELECT nodes FROM params)
  AND t.fab_ops * 10 > t.all_ops

UNION ALL

SELECT 'transport_mix has no operations at all, so neither claim above was actually tested',
       (SELECT npool FROM params), (SELECT nodes FROM params),
       t.fab_ops, t.local_ops, t.all_ops
FROM t
WHERE t.all_ops = 0

UNION ALL

SELECT 'every UCT operation took a fabric transport, though four ranks share each node',
       (SELECT npool FROM params), (SELECT nodes FROM params),
       t.fab_ops, t.local_ops, t.all_ops
FROM t
WHERE t.all_ops > 0 AND t.local_ops = 0;
