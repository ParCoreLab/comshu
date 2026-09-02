-- requires: link_health
-- The link's own rubric must report no finding.
--
-- QUERIED AS `n > 0`, NOT AS `count(*) = 0`. BRIEF-BLACKBOX.md's first edition
-- said link_health "is expected to be empty. Every row in it is a defect"; that
-- was corrected on 2026-08-29 after 04_llmc measured it. It is a FIXED RUBRIC:
-- one row per finding KIND, always written, with a count, so a healthy capture
-- has every row present with n = 0 and a reader following the original wording
-- would file eighteen defects against a clean capture. This is the working form,
-- and it is what tests/hpc/cases/21_ucx_deep/job.sbatch uses.
--
-- WHAT IT CATCHES. Any of the link's own consistency findings firing on a real
-- application: an unnamed UCT transport, an unanchored timeline span, a
-- work-request flight time running backwards. None of those can be diagnosed
-- from the derived tables themselves, which is why the rubric exists, and none
-- of this study's other assertions would notice them.
--
-- WHAT IT CANNOT CATCH, recorded here because the limitation is the finding.
-- The rubric's vocabulary is not published anywhere a black-box reader can see,
-- so `n = 0` on five rows and `n = 0` on twenty rows look equally clean and
-- there is no way to assert COMPLETENESS. Clause 2 does the little that can be
-- done -- if the table is empty rather than a rubric of zeroes, something did
-- not run -- and the rest is in QUESTIONS.md.
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
