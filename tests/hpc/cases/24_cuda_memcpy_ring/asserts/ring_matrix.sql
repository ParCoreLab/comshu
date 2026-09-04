-- requires: comm_edges edge_sizes
-- THE RING, IN RANK SPACE. Rank r sends exactly `iters` copies of `msg_bytes`
-- to rank (r+1) mod ranks, and no other copy edge exists.
--
-- Built as a LEFT JOIN FROM THE EXPECTED RING, not as a filter over the
-- capture, and that is the whole design of this file. The suite's usual
-- "offending rows, empty = pass" shape cannot state a MISSING edge: on a
-- capture with no copies at all it selects from nothing and goes green. Here
-- the recursive CTE generates the ring params implies, so an edge the capture
-- does not have is a row with NULLs and the assertion fails. The second half
-- catches the opposite — an edge the ring does not predict.
--
-- No rank and no ordinal is named: the ring comes from `ranks`, the volume from
-- `iters` and `msg_bytes`.
WITH RECURSIVE r(i) AS (
  SELECT 0
  UNION ALL SELECT i + 1 FROM r WHERE i + 1 < (SELECT ranks FROM params)
),
expected AS (
  SELECT i AS src, (i + 1) % (SELECT ranks FROM params) AS dst FROM r
),
got AS (
  SELECT src_rank, dst_rank, sends, bytes
  FROM comm_edges WHERE transport = 'cudamemcpy'
)
SELECT 'missing or wrong' AS problem, e.src, e.dst, g.sends, g.bytes
FROM expected e
LEFT JOIN got g ON g.src_rank = e.src AND g.dst_rank = e.dst
WHERE g.sends IS NULL
   OR g.sends != (SELECT iters FROM params)
   OR g.bytes != (SELECT CAST(iters AS BIGINT) * msg_bytes FROM params)
UNION ALL
SELECT 'unexpected edge', g.src_rank, g.dst_rank, g.sends, g.bytes
FROM got g
WHERE NOT EXISTS (SELECT 1 FROM expected e
                   WHERE e.src = g.src_rank AND e.dst = g.dst_rank);
