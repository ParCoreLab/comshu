-- requires: ucx_tag_send params.ompi_rank_shift params.ompi_rank_mask
-- Every tag a rank sends must carry that rank's own number, because Open MPI
-- puts it there.
--
-- THE ORACLE IS OPEN MPI'S SOURCE, NOT A CAPTURE.
-- ompi/mca/pml/ucx/pml_ucx_request.h:42, v4.1.x, which is the MPI gmx_mpi is
-- linked against (round 1's NOTES.md: libmpi.so.40 out of HPC-X 2.16):
--
--     PML_UCX_MAKE_SEND_TAG(tag, comm) =
--         ((uint64_t)tag             << (RANK_BITS + CONTEXT_BITS)) |
--         ((uint64_t)comm->c_my_rank << CONTEXT_BITS)               |
--         ((uint64_t)comm->c_contextid)
--
--     PML_UCX_TAG_BITS 24   PML_UCX_RANK_BITS 20   PML_UCX_CONTEXT_BITS 20
--
-- so bits 20-39 of every UCP tag an Open MPI rank sends are that rank's index in
-- the communicator, and :65 PML_UCX_TAG_GET_SOURCE is exactly the shift-and-mask
-- below. Every send GROMACS makes goes through the PML, so this holds for the
-- halo, for PP<->PME and for the collectives alike -- there is no exempt path.
--
-- WHY IT MATTERS FAR MORE THAN IT LOOKS. Round 1's F2 established that the only
-- documented cross-rank join in the UCX records is broken on Open MPI 4.1.x:
-- ucp_worker_query is not hooked, so ucx_worker_address records the wrong one of
-- the two workers each rank creates and 61 of 64 resolved endpoints carry one
-- byte. The conclusion drawn was that rank-space topology is unavailable for an
-- HPC-X application. It is not: for ANY Open MPI application the peer is inside
-- the tag, and needs no join between rank directories at all. This assertion is
-- what establishes that the tag comshu recorded is the tag Open MPI built, and
-- halo_and_pme_peers_match_the_decomposition.sql is what then spends it.
--
-- THE SECOND CLAUSE IS A DECODE CANARY AND IT IS NOT OPTIONAL. The CSV export
-- renders these fields as hex strings and the Parquet export as integers -- case
-- 21's tag_and_addr_are_door_scoped.sql documents having to be written for both
-- -- so arithmetic written for one engine silently matches nothing on the other
-- and every tag-based assertion in this directory then passes while testing
-- nothing. If NOT ONE row in a non-empty table decodes to its own rank, the
-- cause is the representation, not the tracer, and the assertion says so by
-- name rather than going green.
SELECT 'a rank sent a tag whose encoded source is not that rank' AS problem,
       CAST(rank AS INTEGER) AS rank,
       ((CAST(tag AS BIGINT) >> (SELECT ompi_rank_shift FROM params))
        & (SELECT ompi_rank_mask FROM params)) AS encoded_source,
       count(*) AS rows
FROM ucx_tag_send
WHERE ((CAST(tag AS BIGINT) >> (SELECT ompi_rank_shift FROM params))
       & (SELECT ompi_rank_mask FROM params)) <> CAST(rank AS INTEGER)
GROUP BY CAST(rank AS INTEGER),
         ((CAST(tag AS BIGINT) >> (SELECT ompi_rank_shift FROM params))
          & (SELECT ompi_rank_mask FROM params))

UNION ALL

SELECT 'no send tag decoded to its own rank at all - the tag column is not an integer here',
       -1,
       -1,
       (SELECT count(*) FROM ucx_tag_send)
WHERE (SELECT count(*) FROM ucx_tag_send) > 0
  AND NOT EXISTS (
    SELECT 1 FROM ucx_tag_send
    WHERE ((CAST(tag AS BIGINT) >> (SELECT ompi_rank_shift FROM params))
           & (SELECT ompi_rank_mask FROM params)) = CAST(rank AS INTEGER)
  );
