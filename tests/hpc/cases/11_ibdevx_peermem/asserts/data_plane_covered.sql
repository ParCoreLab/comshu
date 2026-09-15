-- requires: mlx5_devx_qp
-- The watcher saw all of it, and its own account agrees with the capture.
--
-- Every rank that created DEVX queue pairs must have exactly one send-queue
-- watch coverage row (mlx5_send_coverage), and that row must say:
--
--   nothing lapped, nothing dropped   this workload is ~2,000 WQEs a rank, far
--                                     inside what the watcher keeps whole
--                                     (lossless to 204,800 a rank on du04)
--   decoded == published, and > 0     everything it saw published it decoded
--   refused == 0, queues > 0          no queue pair it declined to watch
--   rows == this rank's sq_watch rows its count and the capture's agree, so a
--                                     row lost between decode and disk shows
--
-- suite/asserts/send_coverage_adds_up.sql already holds published == decoded +
-- lapped + dropped on every run; this is the case's own, stricter claim that the
-- lapped and dropped terms are zero here.
--
-- ABSENCE IS THE FAILURE THIS GUARDS FIRST. The coverage row is written by the
-- watcher's atexit handler, before the tracer finalizes; a rank with DEVX queue
-- pairs and no row either ran without --watch-send-queues or never reached a
-- normal exit. So `requires: mlx5_devx_qp` — requiring mlx5_send_coverage would
-- turn a missing row into a [skip], which is the one outcome this must not have.
--
-- An invariant that names no rank, queue pair or device.
WITH cov AS (
    SELECT CAST(rank AS INTEGER)            AS rank,
           count(*)                          AS coverage_rows,
           sum(CAST(published AS INTEGER))   AS published,
           sum(CAST(decoded AS INTEGER))     AS decoded,
           sum(CAST(lapped AS INTEGER))      AS lapped,
           sum(CAST(dropped AS INTEGER))     AS dropped,
           sum(CAST(rows AS INTEGER))        AS rows_claimed,
           sum(CAST(queues AS INTEGER))      AS queues,
           sum(CAST(refused AS INTEGER))     AS refused
    FROM mlx5_send_coverage
    WHERE submitter_name = 'sq_watch'
    GROUP BY CAST(rank AS INTEGER)
),
sent AS (
    SELECT CAST(rank AS INTEGER) AS rank, count(*) AS n
    FROM mlx5_device_send
    WHERE submitter_name = 'sq_watch'
    GROUP BY CAST(rank AS INTEGER)
),
qp_ranks AS (
    SELECT DISTINCT CAST(rank AS INTEGER) AS rank FROM mlx5_devx_qp
)
SELECT q.rank,
       c.coverage_rows,
       c.published, c.decoded, c.lapped, c.dropped,
       c.rows_claimed, coalesce(s.n, 0) AS rows_in_capture,
       c.queues, c.refused
FROM qp_ranks q
LEFT JOIN cov  c ON c.rank = q.rank
LEFT JOIN sent s ON s.rank = q.rank
WHERE c.coverage_rows IS NULL
   OR c.coverage_rows != 1
   OR c.lapped != 0
   OR c.dropped != 0
   OR c.decoded != c.published
   OR c.decoded = 0
   OR c.refused != 0
   OR c.queues = 0
   OR c.rows_claimed != coalesce(s.n, 0);
