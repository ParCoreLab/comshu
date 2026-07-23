-- GPU-initiated accesses by op. NCCL's NVLink P2P copies show up as peer
-- load/store instructions instrumented by NVBit. Deterministic given pinned
-- NCCL (Ring/Simple, fixed channels) + fixed size/iters.
SELECT rank, op, count(*) AS n, sum(CAST(size AS INTEGER)) AS bytes
FROM gpu_access
GROUP BY rank, op
ORDER BY rank, op;
