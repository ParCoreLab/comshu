-- requires: transport_mix
-- `transport_mix.scope` must be the function of `tl_name` the contract says it
-- is. This is the only file in this directory that compares comshu against comshu's
-- own published documentation rather than against LAMMPS.
--
-- THE CONTRACT, from BRIEF-BLACKBOX.md (published 2026-08-30, amended
-- 2026-08-31 to add gdr_copy):
--
--   intra-node : self sysv posix mm cma knem xpmem cuda_ipc cuda_copy gdr_copy
--                rocm_ipc rocm_copy rocm_gdr
--   network    : rc_verbs rc_mlx5 dc_mlx5 ud_verbs ud_mlx5 ib srd tcp
--   unknown    : anything else
--
-- WHY IT IS NOT CIRCULAR. network_share_of_wire_bytes.sql states LAMMPS's
-- decomposition in comshu's vocabulary, which is right -- `scope` is the column
-- the contract provides for "did this leave the node", and expressing the claim
-- that way keeps it true on a fabric that is not mlx5. But it does mean that
-- assertion inherits the mapping's correctness. This file is where the mapping
-- itself is pinned, so that a failure over there can be attributed.
--
-- A ROW HERE MEANS THE DOCUMENTATION AND THE IMPLEMENTATION DISAGREE, which is
-- worse than a missing contract because a reader who checks it is misled rather
-- than merely stuck. This round already has one instance on record -- the brief
-- says `link_health` should be empty while the table is a fixed rubric of
-- zero-valued rows -- so it is a pattern rather than a hypothetical.
--
-- WHAT MAKES IT NON-VACUOUS HERE. Four ranks per node over four nodes is the
-- first LAMMPS configuration in this project that must produce BOTH families at
-- once: the intra-node clause has rows to check because three of every four
-- peers... no: two of every four peers of every rank share its node in the
-- `block` arm and two do not, so `sysv`/`posix`/`cma` and `rc_mlx5`/`dc_mlx5`
-- must both appear. Clause 4 asserts exactly that, so the first three clauses
-- cannot pass over an empty relation.
SELECT 'a transport the contract calls intra-node carries a different scope' AS problem,
       tl_name, dev_name, scope, sum(CAST(ops AS BIGINT)) AS ops
FROM transport_mix
WHERE tl_name IN ('self','sysv','posix','mm','cma','knem','xpmem',
                  'cuda_ipc','cuda_copy','gdr_copy','rocm_ipc','rocm_copy','rocm_gdr')
  AND (scope IS NULL OR scope <> 'intra-node')
GROUP BY tl_name, dev_name, scope

UNION ALL

SELECT 'a transport the contract calls network carries a different scope',
       tl_name, dev_name, scope, sum(CAST(ops AS BIGINT))
FROM transport_mix
WHERE tl_name IN ('rc_verbs','rc_mlx5','dc_mlx5','ud_verbs','ud_mlx5','ib','srd','tcp')
  AND (scope IS NULL OR scope <> 'network')
GROUP BY tl_name, dev_name, scope

UNION ALL

SELECT 'scope took a value outside the three the contract declares',
       tl_name, dev_name, scope, sum(CAST(ops AS BIGINT))
FROM transport_mix
WHERE scope IS NULL OR scope NOT IN ('intra-node','network','unknown')
GROUP BY tl_name, dev_name, scope

UNION ALL

-- The anti-vacuity clause. If either family is missing entirely, the three
-- checks above assert nothing and the run did not do what this study needs.
SELECT 'a whole scope family is missing from transport_mix', '-', '-',
       CASE WHEN (SELECT count(*) FROM transport_mix WHERE scope = 'intra-node') = 0
            THEN 'no intra-node rows' ELSE 'no network rows' END, 0
WHERE (SELECT count(*) FROM transport_mix WHERE scope = 'intra-node') = 0
   OR (SELECT count(*) FROM transport_mix WHERE scope = 'network')    = 0;
