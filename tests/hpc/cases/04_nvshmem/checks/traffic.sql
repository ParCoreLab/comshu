-- Device-side nvshmem_int_p stores to the peer's symmetric heap, by op.
-- Deterministic: fixed PE count, fixed element count, fixed iters.
SELECT rank, op, count(*) AS n, sum(CAST(size AS INTEGER)) AS bytes
FROM gpu_access
GROUP BY rank, op
ORDER BY rank, op;
