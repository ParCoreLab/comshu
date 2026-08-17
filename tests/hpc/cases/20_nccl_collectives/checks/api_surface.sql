-- GOLDEN — the shape of what the provider captured, per operation.
--
-- Characterization, not correctness: the assertions next door check the counts
-- against the workload and the ring against its definition. What this catches is
-- a change nobody asked for — an op that stops being recorded, a datatype
-- decoded differently, a logical_bytes derivation that shifts.
--
-- logical_bytes is included ON PURPOSE despite being derived, because the
-- derivation is exactly the thing worth pinning: it is count x the datatype
-- width, and the column is named `logical_` because it is what the application
-- handed over and NOT what crosses the wire. If someone later renames it to
-- `bytes` or starts multiplying it by a ring factor, this golden goes red.
--
-- Summed across ranks, so a per-rank ordering change is not a diff.
SELECT op, datatype, elem_bytes, count(*) AS calls,
       sum(CAST(count AS BIGINT)) AS elements,
       sum(CAST(logical_bytes AS BIGINT)) AS logical_bytes
FROM nccl_collective
GROUP BY op, datatype, elem_bytes
ORDER BY op;
