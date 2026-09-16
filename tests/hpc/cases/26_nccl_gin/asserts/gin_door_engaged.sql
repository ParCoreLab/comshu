-- requires: mlx5_devx_qp params.ranks
-- THE GATE. Every rank's doorbell door engaged and decoded something.
--
-- gin_puts_arrived is the claim this case exists for, but on a run where the
-- door never armed it fails with "0 of 2,621,440 bytes" on every rank, which
-- reads like a decode failure and is not one — it is a platform or flag
-- problem, and the two want different fixes. This says which: no doorbells at
-- all means nothing was watched.
--
-- Why not `requires: mlx5_doorbell`: a table that is empty because the door
-- never armed is exactly the case being caught, and requiring it would turn
-- that into a [skip]. mlx5_devx_qp is required instead — every GIN run creates
-- DEVX queues whether or not a door watched them, so its absence means the
-- capture itself is broken and tables_present says so first.
--
-- Counts are deliberately not asserted. How many doorbells a batch of puts
-- becomes is the transport's business and moves with timing: measured 3,146 on
-- one rank and 320 on the other in the same run (MN5 job 45913525), and 640 on
-- both in the next. What is invariant is that neither is zero.
SELECT r.rank, coalesce(d.n, 0) AS doorbells
FROM ranks_seen r
LEFT JOIN (
    SELECT CAST(rank AS INTEGER) AS rank, count(*) AS n
    FROM mlx5_doorbell GROUP BY CAST(rank AS INTEGER)
) d ON d.rank = r.rank
WHERE coalesce(d.n, 0) = 0

UNION ALL

SELECT -1, (SELECT count(*) FROM ranks_seen)
WHERE (SELECT count(*) FROM ranks_seen) != (SELECT ranks FROM params);
