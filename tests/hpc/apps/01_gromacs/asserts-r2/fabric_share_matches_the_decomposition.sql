-- requires: transport_mix params.fabric_pct_lo params.fabric_pct_hi
-- THE HEADLINE. The share of wire bytes that left the node must be the share
-- GROMACS's domain decomposition says it must be.
--
-- THE ORACLE, AND IT NEEDS NO MEASUREMENT. With `-npme 1 -dd 7 1 1` on 8 ranks
-- there are exactly 13 undirected communication edges: a 7-cycle of halo edges
-- among the PP ranks (domdec/domdec.cpp:1062 setup_neighbor_relations, which
-- gives cell x the neighbours (x+-1) mod 7 and no others at one pulse) and a
-- 7-spoke star from every PP rank to the PME rank (ewald/pme_pp.cpp:296).
-- `mpirun --map-by ppr:4:node` puts ranks 0-3 on one node and 4-7 on the other,
-- so 4 of the 7 PME spokes cross and 2 of the 7 halo edges do.
--
-- Write p for the total PP<->PME volume per step and h for the total halo
-- volume. The share that leaves the node is
--
--     f = (4/7 . p + 2/7 . h) / (p + h)
--
-- which for ANY p, h > 0 lies strictly between 2/7 = 28.6% and 4/7 = 57.1%.
-- The relative weight of halo and PME never has to be known. The band in
-- meta.json is that interval widened by a couple of points for wireup and
-- collectives, which are kilobytes against gigabytes. The point estimate, using
-- the one halo figure GROMACS prints in its own log, is 50.0%.
--
-- WHAT IT CATCHES. This is the question comm_edges cannot answer -- and on the
-- HPC-X stack comm_edges cannot answer anything at all, because UCX rings its
-- own doorbell and ib_post_send is never called (round 1, F1). So transport_mix
-- is not merely a better witness here, it is the only one. A capture that puts
-- 100% on the network has not found the shared-memory interfaces; one that puts
-- 0% there has lost the InfiniBand ones; one that lands at 29% has lost the PME
-- traffic and one at 57% has lost the halo. Every one of those is a live failure
-- mode for a layer nobody has ever pointed at an application.
--
-- GROUPED BY `scope`, WHICH IS THE COLUMN THAT ANSWERS THIS. The brief's
-- contract (corrected 2026-08-30) gives scope exactly three values --
-- 'intra-node', 'network', 'unknown' -- assigned from tl_name by the link. That
-- is pcomm's own answer to "did this leave the node", and asking it in pcomm's
-- vocabulary is what makes the claim portable across fabrics: a run over srd or
-- tcp or plain ud_verbs is still `network`, where a tl_name pattern would have
-- missed all three. Whether the scope mapping is itself applied correctly is a
-- separate question and has its own file, scope_matches_the_documented_mapping.sql
-- -- so this assertion is not asking pcomm to mark its own homework, it is
-- asking whether pcomm's answer matches GROMACS's decomposition.
--
-- out_bytes + in_bytes and NOT advertised_bytes: case 21's
-- rendezvous_advertises_rather_than_sends.sql states that a rendezvous
-- operation's payload is offered rather than sent, and folding it in would bill
-- the same buffer twice -- once as this rank's advertisement and again as the
-- peer's Get, which is where it lands as in_bytes.
WITH t AS (
  SELECT sum(CASE WHEN scope = 'network'    THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END) AS net,
         sum(CASE WHEN scope = 'intra-node' THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END) AS shm,
         sum(CASE WHEN scope NOT IN ('network','intra-node')
                   OR scope IS NULL         THEN CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) ELSE 0 END) AS unk
  FROM transport_mix
)
SELECT 'transport_mix recorded no wire bytes in either scope' AS problem,
       net, shm, unk, 0 AS network_pct
FROM t
WHERE net + shm = 0

UNION ALL

SELECT 'the network share of wire bytes is outside the range the decomposition allows',
       net, shm, unk, (100 * net) / (net + shm)
FROM t
WHERE net + shm > 0
  AND (    (100 * net) / (net + shm) < (SELECT fabric_pct_lo FROM params)
        OR (100 * net) / (net + shm) > (SELECT fabric_pct_hi FROM params) )

UNION ALL

-- Bytes in `unknown` scope are bytes this fraction cannot account for, so the
-- ratio above is being computed over an incomplete denominator and must not be
-- believed. A material share there means the link met a transport its mapping
-- does not name -- which for a GROMACS run over HPC-X should be none of them.
SELECT 'a material share of wire bytes is in unknown scope',
       net, shm, unk, (100 * unk) / (net + shm + unk)
FROM t
WHERE net + shm + unk > 0
  AND (100 * unk) / (net + shm + unk) > (SELECT unclassified_max_pct FROM params);
