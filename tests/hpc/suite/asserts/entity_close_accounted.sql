-- requires: entity_close
-- Every close names an entity some open record created, and closes never
-- outnumber opens for one subject.
--
-- `subject` is the interned handle the open record carries, and interned handles
-- are unique across providers, so this join is exact and needs no provider
-- knowledge: a memory region, a queue pair, a umem and a UAR are all checked by
-- the same two clauses.
--
-- WHAT AN UNMATCHED CLOSE MEANS. Not a bug in the close hook. It is the mirror
-- of case 08's unaccounted queue pair — a transition referring to a queue pair
-- nothing created — arriving from the other side: evidence of a CREATION door we
-- do not hook. That case took a week to find because the only symptom was an
-- empty table, and six hypotheses died to instruments that answer questions
-- about symbols while the answer was a function pointer.
--
-- So these rows are the cheapest possible detector for the next missing door,
-- and the temptation when one fires will be to filter it out. Don't. Find the
-- door. Suppressing the row discards the only signal that would have found it.
--
-- Scoped by rank throughout. Handles are per-process counters, so handle 3 on
-- rank 0 and handle 3 on rank 1 are unrelated — the same discipline comm_edges
-- learned from qp_num, which is HCA-local and made a qp_num-keyed map resolve
-- every colliding peer to whichever rank was scanned last.
--
-- Names no rank, no handle and no count.
-- `opener` is the type_id of the record that should have created the subject,
-- so an unmatched row names the door to go looking for instead of merely saying
-- something is missing: 100 ib_mem_region, 102 ib_queue_pair, 200 mlx5_umem,
-- 202 mlx5_uar.
WITH opens AS (
  SELECT rank, mr AS subject, 100 AS opener FROM ib_mem_region
  UNION ALL SELECT rank, qp, 102 FROM ib_queue_pair
  UNION ALL SELECT rank, umem, 200 FROM mlx5_umem
  UNION ALL SELECT rank, uar, 202 FROM mlx5_uar
),
closes AS (SELECT rank, subject, opener, count(*) AS n
           FROM entity_close GROUP BY rank, subject, opener),
open_n AS (SELECT rank, subject, count(*) AS n FROM opens GROUP BY rank, subject)

SELECT 'close names an entity no open record created (opener ' || c.opener || ')' AS problem,
       c.rank, c.subject, c.n AS closes, 0 AS opens
FROM closes c
WHERE NOT EXISTS (SELECT 1 FROM open_n o WHERE o.rank = c.rank AND o.subject = c.subject)

UNION ALL

-- More closes than opens for one subject. A handle is a pointer value, so it CAN
-- legitimately repeat once the address is reused — but only after a close, which
-- means opens and closes must interleave and the running count can never go
-- negative. A total that does is either a double-free in the application or a
-- close recorded for something we saw opened under a different handle.
SELECT 'more closes than opens for one subject (opener ' || c.opener || ')',
       c.rank, c.subject, c.n,
       (SELECT n FROM open_n o WHERE o.rank = c.rank AND o.subject = c.subject)
FROM closes c
WHERE c.n > COALESCE((SELECT n FROM open_n o WHERE o.rank = c.rank AND o.subject = c.subject), 0)

ORDER BY 1, 2, 3;
