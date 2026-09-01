-- requires: comm_edges ucx_tag_send params.ranks params.edge_coverage_min_pct
-- Every rank's UCP send volume must reappear in comm_edges, and this file is the
-- guard that stops every other edge assertion in this directory from passing
-- vacuously.
--
-- WHY IT EXISTS. Round 1 of this case (REPORT.md D1, NOTES.md #7) shipped three
-- assertions over the peer-resolved edge relation that all passed while that
-- relation was EMPTY -- 0 of 621 halo messages per rank resolved to a peer, so
-- every "no offending row" check had no rows to offend. The general fix recorded
-- there is: drive the aggregate off the rows that MUST exist, never off the join
-- result. ucx_tag_send is that population. It is the raw UCP door; if LAMMPS ran
-- at all it has rows on all 16 ranks, and it needs no cross-rank join.
--
-- WHAT IT CATCHES. A comm_edges whose UCX lane resolves some peers and not
-- others. That is not hypothetical: it is exactly round 1's failure at 0 %, and
-- 01_gromacs round 1 measured the intermediate case (64 of 117 connections
-- resolved, none of them carrying traffic) where a share-based assertion
-- normalised over the resolved subset would have reported a clean split of
-- nothing.
--
-- ON THE UNIT. sum(ucx_tag_send.count) is bytes here and that is an Open MPI
-- fact rather than a pcomm one: pml_ucx_datatype.h:87 pre-multiplies count by
-- the element size for a contiguous power-of-two datatype, and every halo
-- message LAMMPS sends is MPI_DOUBLE (comm_brick.cpp / comm_kokkos.cpp pass
-- MPI_DOUBLE throughout). It is recorded in QUESTIONS.md because nothing in the
-- capture says so. The threshold is a percentage, so a systematic factor would
-- have to be smaller than 1.11x to hide here, and the only candidate factor in
-- this stack is 8.
SELECT 'a rank sent UCP bytes that reach no comm_edges row' AS problem,
       t.rank, t.ucp_bytes, COALESCE(e.edge_bytes, 0) AS edge_bytes,
       (100 * COALESCE(e.edge_bytes, 0)) / t.ucp_bytes AS pct
FROM (SELECT rank, sum(CAST(count AS BIGINT)) AS ucp_bytes
      FROM ucx_tag_send GROUP BY rank) t
LEFT JOIN (SELECT src_rank, sum(CAST(bytes AS BIGINT)) AS edge_bytes
           FROM comm_edges GROUP BY src_rank) e ON e.src_rank = t.rank
WHERE t.ucp_bytes > 0
  AND (100 * COALESCE(e.edge_bytes, 0)) / t.ucp_bytes
      < (SELECT edge_coverage_min_pct FROM params)

UNION ALL

-- And the population itself must be complete: 16 ranks sent, so 16 ranks must
-- have UCP rows. A capture missing a rank entirely would otherwise sail through
-- every per-rank check above.
SELECT 'not every rank appears in ucx_tag_send',
       (SELECT count(DISTINCT rank) FROM ucx_tag_send), 0, 0, 0
WHERE (SELECT count(DISTINCT rank) FROM ucx_tag_send) <> (SELECT ranks FROM params);
