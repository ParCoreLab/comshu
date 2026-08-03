-- requires: mem_object params.gpus
-- One tracked allocation per GPU: the process cudaMalloc's a buffer on each
-- device before the ring runs, so a correct capture has exactly `gpus` of them
-- and no more. Derived from the GPU count, not counted off a previous run.
--
-- The upper bound is the useful half. Under-counting allocations makes device
-- attribution silently approximate — an access whose address matches no tracked
-- object has to be attributed by some other means — and that is precisely the
-- failure mode 04's fragmented symmetric heap turned into out-of-range mem_dev
-- values.
SELECT rank, count(*) AS objects
FROM mem_object
GROUP BY rank
HAVING objects != (SELECT gpus FROM params)
ORDER BY CAST(rank AS INTEGER);
