-- Completion health: every work request must complete with status=success.
-- A single err* row here is a real failure (dropped/failed transfer).
SELECT rank, status, opcode, count(*) AS n
FROM ib_poll_cq
GROUP BY rank, status, opcode
ORDER BY rank, status, opcode;
