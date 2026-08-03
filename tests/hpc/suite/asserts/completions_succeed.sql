-- requires: ib_poll_cq
-- Not one work request may complete with anything but success. A single err*
-- row is a dropped or failed transfer, and it means every volume number in the
-- rest of the case is describing a broken run.
--
-- The per-case goldens group by status and so *record* the statuses that
-- occurred; recording is not asserting. A golden regenerated on a run with
-- errors in it would happily pin the errors.
--
-- Names no rank: the claim is about all of them.
SELECT rank, status, opcode, count(*) AS n
FROM ib_poll_cq
WHERE status <> 'success'
GROUP BY rank, status, opcode
ORDER BY CAST(rank AS INTEGER), status, opcode;
