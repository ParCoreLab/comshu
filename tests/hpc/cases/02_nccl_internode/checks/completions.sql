-- Completion health across all 8 ranks: every WR must complete with success.
-- Groups by status only (opcode mix is covered by traffic) — one err* row fails.
SELECT rank, status, count(*) AS n
FROM ib_poll_cq
GROUP BY rank, status
ORDER BY rank, status;
