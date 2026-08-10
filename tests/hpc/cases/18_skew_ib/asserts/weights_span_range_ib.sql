-- requires: comm_edges params.ranks
-- NB comm_edges here is the `--csv` export, whose columns are
-- (src_rank, dst_rank, sends, bytes) — no `transport`. That column exists only
-- in the `--link` Parquet. It is not needed: this case loads ib alone with one
-- GPU per node, so there is no NVLink traffic to exclude by construction.
-- The heaviest predicted edge must carry at least 8x the lightest.
--
-- A guard on the FIXTURE, not on the tool. The formula spans 32x by
-- construction; 8x is the floor below which this capture stops exercising the
-- matrix's log colour scale, which a uniform matrix satisfies trivially. If a
-- future PE count flattened the spread this case would still pass everything
-- else while quietly testing nothing — the silent-degradation mode a ring-only
-- corpus already cost us once.
WITH RECURSIVE seq(i) AS (
    SELECT 0 UNION ALL SELECT i+1 FROM seq WHERE i+1 < (SELECT ranks FROM params)
),
expected AS (
    SELECT a.i AS src, b.i AS dst FROM seq a, seq b
    WHERE a.i <> b.i AND ((a.i * 31 + b.i * 17) % 11) >= 5
),
heavy AS (
    SELECT CAST(e.bytes AS INTEGER) b
    FROM comm_edges e JOIN expected x
      ON x.src = CAST(e.src_rank AS INTEGER) AND x.dst = CAST(e.dst_rank AS INTEGER)
)
SELECT 'edge weights too uniform to exercise a log scale' AS problem,
       min(b) AS lightest, max(b) AS heaviest
FROM heavy
HAVING max(b) < 8 * min(b);
