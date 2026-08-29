-- requires: ucx_rma params.msg_min_bytes params.total_ops
-- One-sided means one side. All of it belongs to the origin rank.
--
-- In run_put_with_flush the entire lock / put / flush / unlock body is inside
-- `if (rank == 0)` (osu_put_bw.c:303-335); rank 1 reaches nothing but
-- MPI_Barrier at :337. That is what passive-target RMA IS, and it is the
-- property that makes a one-sided trace hard to read: the target never calls
-- anything, so nothing on its side records the transfer.
--
-- WHAT IT CATCHES, and it is a real hazard rather than a hypothetical. If the
-- provider ever attributed an operation by the rank whose MEMORY was touched
-- rather than the rank that ISSUED the call, every row would still be present,
-- every byte total would still be right, and the traffic would be on the wrong
-- side of the fabric. Aggregate checks cannot see that; this can.
--
-- NAMES NO RANK, which the suite requires. It asserts that the origin set has
-- exactly one member and that that member issued the full total_ops, not that
-- the member is rank 0 — a run that renumbered ranks would still pass, and a
-- run that split the traffic across both would not.
SELECT 'one-sided traffic was attributed to more than one rank' AS problem,
       (SELECT count(DISTINCT rank) FROM ucx_rma
        WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params)) AS origin_ranks,
       1                                                                    AS expected
WHERE (SELECT count(DISTINCT rank) FROM ucx_rma
       WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params)) <> 1
UNION ALL
SELECT 'the origin rank did not issue the number of operations the command line asked for',
       (SELECT count(*) FROM ucx_rma
        WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params)),
       (SELECT total_ops FROM params)
WHERE (SELECT count(*) FROM ucx_rma
       WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params))
      <> (SELECT total_ops FROM params);
