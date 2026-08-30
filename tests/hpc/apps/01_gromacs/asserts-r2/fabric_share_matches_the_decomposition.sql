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
-- volume. The fabric's share is
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
-- 100% on the fabric has not found the shared-memory interfaces; one that puts
-- 0% there has lost the InfiniBand ones; one that lands at 29% has lost the PME
-- traffic and one at 57% has lost the halo. Every one of those is a live failure
-- mode for a layer nobody has ever pointed at an application.
--
-- CLASSIFIED BY UCX'S OWN TRANSPORT NAMES, NOT BY pcomm's `scope` COLUMN, and
-- that is not fussiness. `scope` is pcomm's answer to this question; using it to
-- ask the question would make the assertion agree with itself whatever it said.
-- The names below are UCX's, from `ucx_info -d`, and pcomm does not choose them.
-- `scope` is checked separately, in transport_scope_discriminates.sql.
--
-- out_bytes + in_bytes and NOT advertised_bytes: case 21's
-- rendezvous_advertises_rather_than_sends.sql states that a rendezvous
-- operation's payload is offered rather than sent, and folding it in would bill
-- the same buffer twice -- once as this rank's advertisement and again as the
-- peer's Get, which is where it lands as in_bytes.
WITH m AS (
  SELECT CASE
           WHEN tl_name IN ('rc_mlx5','rc_verbs','dc_mlx5','ud_mlx5','ud_verbs') THEN 'fabric'
           WHEN tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                            'cuda_ipc','cuda_copy','gdr_copy')                   THEN 'intra'
           ELSE 'unclassified'
         END AS kind,
         CAST(out_bytes AS BIGINT) + CAST(in_bytes AS BIGINT) AS wire
  FROM transport_mix
),
t AS (
  SELECT sum(CASE WHEN kind = 'fabric'       THEN wire ELSE 0 END) AS fab,
         sum(CASE WHEN kind = 'intra'        THEN wire ELSE 0 END) AS shm,
         sum(CASE WHEN kind = 'unclassified' THEN wire ELSE 0 END) AS unk
  FROM m
)
SELECT 'transport_mix recorded no wire bytes on any named transport' AS problem,
       fab, shm, unk, 0 AS fabric_pct
FROM t
WHERE fab + shm = 0

UNION ALL

SELECT 'the fabric share of wire bytes is outside the range the decomposition allows',
       fab, shm, unk, (100 * fab) / (fab + shm)
FROM t
WHERE fab + shm > 0
  AND (    (100 * fab) / (fab + shm) < (SELECT fabric_pct_lo FROM params)
        OR (100 * fab) / (fab + shm) > (SELECT fabric_pct_hi FROM params) )

UNION ALL

-- A transport name UCX does not use, carrying real volume, means either the
-- fabric is not what this study assumed or tl_name is not being resolved. Either
-- way the fraction above is being computed over an incomplete denominator and
-- must not be believed.
SELECT 'a material share of wire bytes is on a transport name neither UCX list covers',
       fab, shm, unk, (100 * unk) / (fab + shm + unk)
FROM t
WHERE fab + shm + unk > 0
  AND (100 * unk) / (fab + shm + unk) > (SELECT unclassified_max_pct FROM params);
