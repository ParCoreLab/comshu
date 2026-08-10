-- requires: comm_edges params.ranks
-- The heaviest edges must be exactly the set the workload's formula predicts:
--
--     h = (from*31 + to*17) % 11 ;  edge iff from != to and h >= 5
--
-- Derived here from the same arithmetic, so this is an assertion and not a
-- golden: a golden would agree with whatever was captured, including a capture
-- that dropped half the edges. Threshold-free, as in 16 and 17 — the count of
-- heaviest edges to take is itself computed from the formula.
WITH RECURSIVE seq(i) AS (
    SELECT 0 UNION ALL SELECT i+1 FROM seq WHERE i+1 < (SELECT ranks FROM params)
),
expected AS (
    SELECT a.i AS src, b.i AS dst
    FROM seq a, seq b
    WHERE a.i <> b.i AND ((a.i * 31 + b.i * 17) % 11) >= 5
),
heaviest AS (
    SELECT CAST(src_rank AS INTEGER) src, CAST(dst_rank AS INTEGER) dst
    FROM comm_edges WHERE transport <> 'nvlink'
    ORDER BY CAST(bytes AS INTEGER) DESC
    LIMIT (SELECT count(*) FROM expected)
)
SELECT 'heavy edge not in the workload''s formula' AS problem, src, dst
FROM heaviest EXCEPT SELECT 'heavy edge not in the workload''s formula', src, dst FROM expected
UNION ALL
SELECT 'formula edge missing from the heaviest traffic', src, dst
FROM expected EXCEPT SELECT 'formula edge missing from the heaviest traffic', src, dst FROM heaviest;
