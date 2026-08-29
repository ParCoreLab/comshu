-- requires: ucx_rma
-- A recorded operation that the transport refused is not a transfer.
--
-- UCX's status convention is documented in its public header and is the one
-- fact about the value a black-box reader can rely on: UCS_OK is 0,
-- UCS_INPROGRESS is 1, every error is negative. `ucp_put_nbi` returns OK or
-- INPROGRESS; a negative status means the call was made and the operation did
-- not happen.
--
-- WHY THIS IS NOT REDUNDANT WITH THE COUNT ORACLE, and case 22 is the evidence.
-- Job 45177421 captured 20 atomic operations that all failed with status -5 and
-- would have satisfied any check that counted rows: a refused operation still
-- produces a row, with a plausible size, on the right rank, naming a real key.
-- Counting rows measures how many times the application called; only the status
-- says how many times the fabric agreed. A capture full of refusals is the
-- tracer working perfectly over a workload that did nothing, and that is the
-- most expensive kind of green board there is.
--
-- Tests the raw integer rather than status_class, deliberately: the class
-- vocabulary is not published in any file this study may read, and the CSV and
-- Parquet exports do not agree on whether it is a string or a number.
SELECT 'a one-sided operation was refused by the transport' AS problem,
       rank,
       CAST(status AS INTEGER) AS status,
       count(*)                AS n
FROM ucx_rma
WHERE CAST(status AS INTEGER) < 0
GROUP BY rank, CAST(status AS INTEGER);
