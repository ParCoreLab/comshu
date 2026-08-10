-- requires: comm_edges params.ranks
-- NB comm_edges here is the `--csv` export, whose columns are
-- (src_rank, dst_rank, sends, bytes) — no `transport`. That column exists only
-- in the `--link` Parquet. It is not needed: this case loads ib alone with one
-- GPU per node, so there is no NVLink traffic to exclude by construction.
-- The heaviest ranks-1 edges must be exactly the chain i -> i+1.
--
-- Threshold-free for the reason given in 16's star assertion: a verbs capture
-- carries NVSHMEM's near-all-to-all bootstrap mesh alongside the data, so edge
-- EXISTENCE says almost nothing. Taking exactly as many heaviest edges as the
-- pattern predicts and comparing the sets needs no tuned byte cutoff.
WITH RECURSIVE seq(i) AS (
    SELECT 0 UNION ALL SELECT i+1 FROM seq WHERE i+1 < (SELECT ranks FROM params)
),
expected AS (
    SELECT i AS src, i+1 AS dst FROM seq WHERE i+1 < (SELECT ranks FROM params)
),
heaviest AS (
    SELECT CAST(src_rank AS INTEGER) src, CAST(dst_rank AS INTEGER) dst
    FROM comm_edges
    ORDER BY CAST(bytes AS INTEGER) DESC
    LIMIT (SELECT ranks - 1 FROM params)
)
SELECT 'heavy edge that the chain does not predict' AS problem, src, dst
FROM heaviest EXCEPT SELECT 'heavy edge that the chain does not predict', src, dst FROM expected
UNION ALL
SELECT 'chain link missing from the heaviest traffic', src, dst
FROM expected EXCEPT SELECT 'chain link missing from the heaviest traffic', src, dst FROM heaviest;
