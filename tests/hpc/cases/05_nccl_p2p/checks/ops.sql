-- Op kinds captured per rank. Deliberately NOT counts: NCCL's LL protocol spins
-- on flag polling, so the number of instrumented accesses is genuinely not
-- repeatable (measured 295,044 vs 550,064 loads on rank 0 across two identical
-- runs). What IS invariant is that every rank performs both peer loads and peer
-- stores, which is the property this case is meant to guard.
SELECT rank, op
FROM gpu_access
GROUP BY rank, op
ORDER BY CAST(rank AS INTEGER), op;
