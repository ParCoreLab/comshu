-- requires: ucx_tag_send uct_send uct_binding params.ranks
-- Both UCX layers must have engaged on all 16 ranks, and the UCT binding must
-- have been accepted rather than refused.
--
-- WHY THIS IS THE FIRST THING TO READ. `ucx_deep` patches `uct_iface_ops_t`, an
-- installed header with no ABI promise, and guards the patch with a layout
-- check. If the guard refuses, `uct_send` is empty, `transport_mix` is empty,
-- and network_share_of_wire_bytes.sql and scope_matches_the_documented_mapping
-- both SKIP -- which in a summary reads identically to "the fabric carried
-- nothing". `uct_binding` is the record that exists to tell those apart, and
-- this file turns it into a verdict.
--
-- IT IS A LIVE RISK HERE AND NOT A FORMALITY. LAMMPS on MN5 runs against the
-- SYSTEM UCX 1.20.0 in /lib64, not against the HPC-X UCX every other pcomm case
-- on this machine uses, and round 1 of this case (NOTES.md trap 1) had to append
-- /lib64 to LD_LIBRARY_PATH to stop the loader building a hybrid of HPC-X's
-- libucp with the system's libuct. Round 2 removes that hazard at the root by
-- building the ucx and ucx_deep providers against /usr/include and
-- /usr/lib64/libucp.so -- the ucx-devel-1.20.0 package, i.e. the very headers
-- the running library was compiled from -- so the layout guard is comparing two
-- copies of the same struct. This assertion is what says whether that worked.
--
-- WHAT IT CATCHES: a refused binding (clause 1); a rank whose UCP doors fired
-- and whose UCT doors did not, which would mean the interposition engaged
-- unevenly across ranks (clause 3); and the plain case of a rank that never
-- reached the capture at all (clause 2).
SELECT 'uct_binding was not accepted on some rank' AS problem,
       rank, accepted, 0 AS ucp, 0 AS uct
FROM uct_binding
WHERE CAST(accepted AS INTEGER) <> 1

UNION ALL

SELECT 'not every rank recorded a uct_binding at all',
       (SELECT count(DISTINCT rank) FROM uct_binding), -1, 0, 0
WHERE (SELECT count(DISTINCT rank) FROM uct_binding) <> (SELECT ranks FROM params)

UNION ALL

SELECT 'a rank reached the UCP layer but not the UCT layer',
       s.rank, -1, s.n, COALESCE(u.n, 0)
FROM (SELECT rank, count(*) AS n FROM ucx_tag_send GROUP BY rank) s
LEFT JOIN (SELECT rank, count(*) AS n FROM uct_send GROUP BY rank) u ON u.rank = s.rank
WHERE COALESCE(u.n, 0) = 0;
