-- Device-side nvshmem_int_p stores to the peer's symmetric heap, by op.
-- Deterministic: fixed PE count, fixed element count, fixed iters.
--
-- Stores only, and that is the assertion. put_to_next issues two global
-- accesses per element: a load of the local src[i] and the peer store. Only the
-- store is communication, and gpu_access holds what RemoteFilter kept
-- (run_device != mem_device), so a `load` row here means local memory was
-- attributed to some other GPU. This golden carried 196608 such loads per rank
-- until 2026-08-03 — the symmetric-heap attribution bug, inflating the capture
-- by 2x rather than merely mislabelling it. See peer_topology.sql.
SELECT rank, op, count(*) AS n, sum(CAST(size AS INTEGER)) AS bytes
FROM gpu_access
GROUP BY rank, op
ORDER BY rank, op;
