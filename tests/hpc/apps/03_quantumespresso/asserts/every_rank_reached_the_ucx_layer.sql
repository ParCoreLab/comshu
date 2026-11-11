-- requires: ucx_tag_send params.ranks
-- Every rank must have posted tagged sends through the UCP layer.
--
-- The floor of the case, and the check that separates the four different
-- "nothings" apps/README.md insists on distinguishing. pw.x is a 52 MB Fortran
-- binary launched through `comshu run` under mpirun; an interposition that
-- engages on three ranks and not the fourth looks exactly like a quiet rank, and
-- every rank-space claim in this directory would then be measuring a hole rather
-- than a decomposition.
--
-- Derived from params.ranks. Names no rank, and is stated as "fewer than
-- expected" so what fails is a rank whose interposition never engaged, not a
-- change in how chatty the application happens to be. No count of messages is
-- asserted: MPI_Alltoall is not obliged to be any particular number of
-- ucp_tag_send calls, and OpenMPI's algorithm selection is not ours to freeze.
SELECT 'not every rank posted a UCP tagged send' AS problem,
       (SELECT count(DISTINCT rank) FROM ucx_tag_send) AS seen,
       (SELECT ranks FROM params)                      AS expected
WHERE (SELECT count(DISTINCT rank) FROM ucx_tag_send) < (SELECT ranks FROM params);
