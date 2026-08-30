-- requires: transport_mix
-- `scope` must be the function of `tl_name` that the contract says it is.
--
-- WHY THIS IS A SEPARATE FILE, AND WHY IT IS NOT CIRCULAR. Three of the
-- assertions here state GROMACS's decomposition in terms of `scope`, which is
-- right -- it is the column the contract provides for "did this leave the node",
-- and expressing the claim in pcomm's vocabulary is what keeps it true on a
-- fabric that is not mlx5. But it does mean those three inherit the mapping's
-- correctness. This file is where the mapping itself is checked, against the
-- table published in BRIEF-BLACKBOX.md on 2026-08-30:
--
--   intra-node : self sysv posix mm cma knem xpmem cuda_ipc cuda_copy
--                rocm_ipc rocm_copy
--   network    : rc_verbs rc_mlx5 dc_mlx5 ud_verbs ud_mlx5 ib srd tcp
--   unknown    : anything else
--
-- So: a documented transport carrying a scope other than its documented one, or
-- carrying `unknown`. Between them the two clauses pin every row whose tl_name
-- the contract names, and leave alone the rows whose tl_name it does not --
-- which are legitimately `unknown` and are counted by the headline assertion
-- instead.
--
-- WHAT A FAILURE HERE MEANS, and it is not the same thing as a failure anywhere
-- else in this directory. Every other file compares pcomm against GROMACS. This
-- one compares pcomm against its own published contract, so a row means the
-- documentation and the implementation disagree -- and this round already has
-- one instance of exactly that (`link_health`'s head comment says the table
-- should be empty while the SQL beneath it emits a zero-valued rubric row per
-- finding kind). A contract that is wrong is worse than one that is missing,
-- because a reader who checks it is misled rather than merely stuck.
--
-- cuda_ipc IS THE ROW THAT MATTERS HERE and it is why this is worth asserting
-- on GROMACS specifically. Intra-node GPU-to-GPU traffic under UCX rides
-- cuda_ipc, whose dev_name is neither an HCA nor a memory device. A scope
-- derived from the device name rather than the transport name gets sysv right
-- and cuda_ipc wrong -- and with direct GPU communication on, which is the
-- default for GROMACS 2025 on a CUDA library-MPI build, cuda_ipc is where most
-- of this run's intra-node traffic goes. The direct-off arm is the control:
-- same decomposition, same edges, host transports only. No workload this
-- project has written produces cuda_ipc traffic at all, because every UCX case
-- in the suite runs one rank per node precisely to avoid shared memory.
SELECT 'a transport the contract calls intra-node carries a different scope' AS problem,
       tl_name, dev_name, scope, sum(CAST(ops AS BIGINT)) AS ops
FROM transport_mix
WHERE tl_name IN ('self','sysv','posix','mm','cma','knem','xpmem',
                  'cuda_ipc','cuda_copy','rocm_ipc','rocm_copy')
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

-- `scope` outside its declared domain entirely. The contract says three values
-- and only three; a fourth means the vocabulary has moved and every query
-- written against it -- including the three headline assertions here -- is
-- matching on strings that no longer mean what they did.
SELECT 'scope took a value outside the three the contract declares',
       tl_name, dev_name, scope, sum(CAST(ops AS BIGINT))
FROM transport_mix
WHERE scope IS NULL OR scope NOT IN ('intra-node','network','unknown')
GROUP BY tl_name, dev_name, scope;
