-- requires: comm_edges params.ranks
-- NB comm_edges here is the `--csv` export, whose columns are
-- (src_rank, dst_rank, sends, bytes) — no `transport`. That column exists only
-- in the `--link` Parquet. It is not needed: this case loads ib alone with one
-- GPU per node, so there is no NVLink traffic to exclude by construction.
-- The star must be a star in RANK space, over the NIC.
--
-- THRESHOLD-FREE, and that is the point. A verbs capture is not just the data
-- path: NVSHMEM bootstraps with a near-all-to-all control mesh, measured on case
-- 07 as 43 control edges carrying 644 sends beside 2 data edges carrying
-- 393,240. So "an edge exists between these ranks" is the wrong predicate — it
-- is true for almost every pair. Instead: take exactly as many heaviest edges as
-- the pattern predicts, and require that SET to be the expected one. No byte
-- threshold to tune, and no assumption about how chatty a future NVSHMEM is.
--
-- The expected set is 2*(ranks-1): every spoke pushes to the hub, and the hub
-- answers every spoke. Derived from params, not from the capture.
--
-- CAST because sql_check.sh loads these CSVs into sqlite, where every column is
-- TEXT and "900" sorts above "1000".
WITH RECURSIVE seq(i) AS (
    SELECT 0 UNION ALL SELECT i+1 FROM seq WHERE i+1 < (SELECT ranks FROM params)
),
expected AS (
    SELECT i AS src, 0 AS dst FROM seq WHERE i <> 0     -- spokes -> hub
    UNION ALL
    SELECT 0, i FROM seq WHERE i <> 0                   -- hub -> spokes
),
heaviest AS (
    SELECT CAST(src_rank AS INTEGER) src, CAST(dst_rank AS INTEGER) dst
    FROM comm_edges
    ORDER BY CAST(bytes AS INTEGER) DESC
    LIMIT (SELECT 2 * (ranks - 1) FROM params)
)
SELECT 'heavy edge that the star does not predict' AS problem, src, dst
FROM heaviest EXCEPT SELECT 'heavy edge that the star does not predict', src, dst FROM expected
UNION ALL
SELECT 'star edge missing from the heaviest traffic', src, dst
FROM expected EXCEPT SELECT 'star edge missing from the heaviest traffic', src, dst FROM heaviest;
