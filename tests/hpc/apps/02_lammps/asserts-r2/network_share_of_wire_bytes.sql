-- requires: transport_mix params.net_pct_lo params.net_pct_hi params.unclassified_max_pct
-- The share of WIRE bytes that left the node must be the share the
-- decomposition and the placement together demand -- measured at the transport
-- layer, which knows nothing about ranks.
--
-- WHY BOTH THIS AND scope_follows_the_launcher_placement.sql. They are the same
-- claim answered by two columns that are computed from two different inputs and
-- share no code path that a user can see:
--
--   comm_edges.scope   is about the two RANKS  -- same node or not, from node
--                      membership. It needs the UCP layer and a cross-rank join.
--   transport_mix.scope is about the TRANSPORT -- assigned from tl_name by the
--                      published table in BRIEF-BLACKBOX.md. It needs the UCT
--                      layer and no join at all.
--
-- For both to be right, UCX must ALSO have made the corresponding choice: shared
-- memory for the peers that share a node and mlx5 for the ones that do not. So
-- this file is simultaneously a check on pcomm and a check that the experiment
-- happened -- and in the `cyclic` arm it demands that UCX made the OPPOSITE
-- choice for the SAME peers, which no amount of caching or defaulting can fake.
--
-- THE NUMBERS. PREDICTION-r2.md §3.3: 55.68 % network in the `block` arm,
-- 44.32 % in `cyclic`, exact complements, from ghost-slab volumes that need no
-- measurement. net_pct_lo/hi is that value +-5 points; the two bands do not
-- overlap.
--
-- out_bytes + in_bytes AND NOT advertised_bytes. The brief's byte-column table
-- of 2026-08-30: the three payload columns partition the ops, in_bytes is
-- recorded once at the rank that issued the Get, and advertised_bytes is offered
-- rather than sent -- folding it in would bill a rendezvous buffer twice, once
-- as the sender's advertisement and again as the peer's Get. Summed globally
-- across ranks, out+in therefore counts every transfer exactly once whichever
-- side of it did the work, which matters here because shared memory pulls
-- (cma/knem GET) and InfiniBand pushes.
--
-- WHAT IT CATCHES. 100 % network: the shared-memory interfaces were never
-- found, which is the failure a study running one rank per node cannot see and
-- which every UCX case in tests/hpc/cases/ therefore cannot see. 0 % network:
-- the InfiniBand interfaces were lost. A value that does not move between the
-- two arms: scope is not a function of what UCX chose. And clause 3: bytes in
-- `unknown` scope are bytes the ratio cannot account for, so the denominator is
-- incomplete and the headline must not be believed -- which is exactly how
-- gdr_copy was caught missing from the mapping on 01_gromacs.
-- THE COALESCE IS NOT DECORATION, AND IT WAS FOUND BY THE DELETE-THE-INPUT TEST.
-- On job 45329318 transport_mix came out EMPTY (the UCT binding was refused --
-- see uct_binding_was_accepted.sql) and the first version of this file PASSED,
-- because `sum(...)` over zero rows is NULL, not 0, so `net + shm = 0` evaluated
-- to NULL and the "recorded no wire bytes" clause never fired. An assertion
-- whose headline claim is a ratio, passing on a table with nothing in it, is the
-- exact defect BRIEF-BLACKBOX.md calls the most common in this suite. COALESCE
-- turns the NULLs into zeroes so the first clause fires as it was meant to.
WITH t AS (
  SELECT COALESCE(sum(CASE WHEN scope = 'network'    THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END), 0) AS net,
         COALESCE(sum(CASE WHEN scope = 'intra-node' THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END), 0) AS shm,
         COALESCE(sum(CASE WHEN scope IS NULL OR scope NOT IN ('network','intra-node')
                                       THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END), 0) AS unk
  FROM transport_mix
)
SELECT 'transport_mix recorded no wire bytes in either scope' AS problem,
       net, shm, unk, 0 AS network_pct
FROM t WHERE net + shm = 0

UNION ALL

SELECT 'the network share of wire bytes is outside the band the decomposition allows',
       net, shm, unk, (100 * net) / (net + shm)
FROM t
WHERE net + shm > 0
  AND (    (100 * net) / (net + shm) < (SELECT net_pct_lo FROM params)
        OR (100 * net) / (net + shm) > (SELECT net_pct_hi FROM params) )

UNION ALL

SELECT 'a material share of wire bytes is in unknown scope',
       net, shm, unk, (100 * unk) / (net + shm + unk)
FROM t
WHERE net + shm + unk > 0
  AND (100 * unk) / (net + shm + unk) > (SELECT unclassified_max_pct FROM params);
