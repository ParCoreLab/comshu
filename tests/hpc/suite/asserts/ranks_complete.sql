-- requires: ranks_seen params.ranks
-- Every rank the workload was launched with recorded something, and nothing
-- recorded under a rank that was never launched.
--
-- The failure this is for: a capture that is short a rank still produces a
-- perfectly self-consistent set of tables, and every other check aggregates by
-- rank and so simply has one group fewer. `comshu` falls back to the pid when
-- COMSHU_RANK is unset and the launcher exports nothing, which turns "8 ranks"
-- into eight five-digit rank ids — the upper half of this query is what
-- notices.
--
-- Derived: the rank count comes from the workload, and no rank id is named.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT 'silent' AS problem, r AS rank FROM expected
WHERE r NOT IN (SELECT rank FROM ranks_seen)
UNION ALL
SELECT 'not launched', rank FROM ranks_seen
WHERE rank < 0 OR rank >= (SELECT ranks FROM params)
ORDER BY 1, 2;
