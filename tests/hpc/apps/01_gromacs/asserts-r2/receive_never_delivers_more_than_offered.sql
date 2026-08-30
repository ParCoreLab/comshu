-- requires: tag_recv_resolved
-- A receive cannot deliver more bytes than the capacity it offered.
--
-- THE CHEAPEST REAL CHECK IN THIS DIRECTORY, and the only one that needs no
-- oracle from the application at all. UcxTagRecv.count is documented as "the
-- capacity offered, not bytes delivered", and UcxTagProbe.length as "the one
-- true byte count in the UCX family"; tag_recv_resolved exists to put the two in
-- one row as `offered` and `delivered`. Whatever the join is, delivered > offered
-- is a buffer overrun the application would have crashed on. If the column pair
-- ever shows it, the join has paired a receive with the wrong probe -- which is
-- exactly the risk the record comment names, since "UCX recycles the handle" and
-- a message id is unique only within a lifetime.
--
-- DELIBERATELY SEPARATE from delivered_length_needs_a_probe.sql. That file needs
-- ucx_tag_probe to exist, and this run is predicted to produce no probes at all
-- (GROMACS calls neither MPI_Mprobe nor MPI_Improbe, so Open MPI never reaches
-- ucp_tag_probe_nb). A `-- requires:` on an absent table skips the whole file,
-- so keeping the two clauses in one file would have made this invariant --
-- which needs nothing but tag_recv_resolved -- skip for want of a table it does
-- not use.
SELECT 'a receive delivered more bytes than it offered' AS problem,
       rank, worker, message, status, offered, delivered
FROM tag_recv_resolved
WHERE delivered IS NOT NULL
  AND offered IS NOT NULL
  AND CAST(delivered AS BIGINT) > CAST(offered AS BIGINT);
