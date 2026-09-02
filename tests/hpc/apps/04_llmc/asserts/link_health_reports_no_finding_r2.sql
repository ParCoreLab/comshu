-- requires: link_health
-- The link must report no finding against this capture.
--
-- apps/BRIEF-BLACKBOX.md says `link_health` "is expected to be empty. Every row
-- in it is a defect in the link". THAT DESCRIPTION IS WRONG, and discovering so
-- is part of this round's result: `link_health` is a fixed rubric of 18 named
-- findings that is always present in full, and health is `n = 0` on every one
-- of them, not the absence of rows. A user who followed the brief literally
-- would file 18 defects against a clean capture.
--
-- So the assertion is the one case 21 already writes (its
-- every_uct_operation_has_a_transport.sql tests `n > 0` for a single finding),
-- generalised to all 18: any finding with a non-zero count is a defect in the
-- link, not in the workload.
--
-- WHAT IT CATCHES on this workload specifically: `unresolved_nvlink` (this
-- capture records node identity but no GPU accesses), `timeline_unanchored`
-- (kernel rows whose clock domain has no anchor — with -p kernel there are 5004
-- of them per capture and two anchors per rank, so an anchor regression shows
-- up here first), `kernel_rows_submission_only` (the schema warns that
-- submission and execution differ by 9-13 ms on kernels lasting 11-20 us, and
-- this is the only place the capture says which it recorded), and
-- `wr_flight_backwards`. All four are zero today.
--
-- Names no finding and no count; it selects whatever is non-zero and prints the
-- link's own note for it.
--
-- SEVERITY FILTER ADDED 2026-09-02. link_health grew a `severity` column of
-- 'defect' or 'note', and the clean predicate is now `n > 0 AND severity =
-- 'defect'`. Without the filter this assertion fires on rows that are not
-- defects at all -- most sharply wr_flight_selective_signalling_qps and the
-- unretired posts beside it, which are a queue pair's own declaration that it
-- does not signal every work request. See link_sql.hpp for the rule that
-- decides which side a row lands on.
SELECT finding, severity, n, note
FROM link_health
WHERE CAST(n AS INTEGER) > 0 AND severity = 'defect'
ORDER BY finding;
