-- requires: link_health
-- The link's own rubric must report no finding.
--
-- QUERIED AS `n > 0`, NOT AS `count(*) = 0`. BRIEF-BLACKBOX.md's first edition
-- said link_health "is expected to be empty. Every row in it is a defect"; that
-- was corrected on 2026-08-29 after 04_llmc measured it to be a FIXED RUBRIC of
-- eighteen named findings, always written in full, each with a count -- so a
-- healthy capture has every row present with n = 0 and a reader following the
-- original wording files eighteen defects against a clean run.
--
-- WHAT MAKES IT WORTH RE-ASSERTING HERE RATHER THAN CARRYING 04_llmc's RESULT.
-- That study ran `-p nccl,kernel` on one node, so the ibverbs-side findings in
-- the rubric were unreachable: there were no queue pairs, no work requests and
-- no completions to be inconsistent about. This capture has all three, at four
-- nodes, so the verbs half of the rubric is exercised for the first time on a
-- third-party application. If a finding fires it will be one of those.
--
-- IT IS ALSO THE ONLY CHECK IN THIS DIRECTORY THAT COULD NOTICE A SILENTLY
-- BROKEN LINK. 04_llmc's rank_identity.device defect was invisible to
-- link_health -- eighteen zeroes while a key column was null on every row -- so
-- a clean rubric is necessary and demonstrably not sufficient, and clause 2 is
-- the little that can be done about the sufficiency half: an EMPTY table means
-- the rubric did not run, which is different from a clean one and would
-- otherwise pass.
--
-- SEVERITY FILTER ADDED 2026-09-02. link_health grew a `severity` column of
-- 'defect' or 'note', and the clean predicate is now `n > 0 AND severity =
-- 'defect'`. Without the filter this assertion fires on rows that are not
-- defects at all -- most sharply wr_flight_selective_signalling_qps and the
-- unretired posts beside it, which are a queue pair's own declaration that it
-- does not signal every work request. See link_sql.hpp for the rule that
-- decides which side a row lands on.
SELECT 'a link_health finding fired' AS problem, finding, n, note
FROM link_health WHERE CAST(n AS BIGINT) > 0 AND severity = 'defect'

UNION ALL

SELECT 'link_health is empty, so the rubric did not run', '-', 0, '-'
WHERE (SELECT count(*) FROM link_health) = 0;
