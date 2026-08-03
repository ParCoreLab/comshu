-- requires: ib_post_send params.ranks params.iters params.msg_bytes
-- `ibv_rc_pingpong -n 20` posts exactly 20 sends of 4096 B on each side — the
-- oracle is the command line, not a previous capture. Both endpoints run the
-- same loop, so this is per rank.
--
-- The golden next door says the same thing in the form "here are the numbers".
-- This says it in the form "here is the derivation", which is the difference
-- between a check that would have caught a dropped send and one that would have
-- been regenerated around it.
--
-- Written as "walk the ranks the workload was launched with and ask each one
-- what it posted", rather than as GROUP BY over the table. A rank that posted
-- NOTHING forms no group, so a HAVING clause cannot see it: verified by
-- mutation, deleting every one of rank 1's sends leaves a GROUP BY form green
-- while the golden catches it. Deriving the rank list from `params` is what
-- makes the assertion able to fail on absence.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM ib_post_send s
          WHERE CAST(s.rank AS INTEGER) = r) AS sends,
         (SELECT coalesce(sum(CAST(s.length AS INTEGER)), 0) FROM ib_post_send s
          WHERE CAST(s.rank AS INTEGER) = r) AS bytes
  FROM expected
)
WHERE sends != (SELECT iters FROM params)
   OR bytes != (SELECT iters * msg_bytes FROM params)
ORDER BY rank;
