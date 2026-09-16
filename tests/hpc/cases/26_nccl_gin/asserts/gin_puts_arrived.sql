-- requires: mlx5_devx_qp params.ranks params.chunk_bytes params.puts_per_iter params.iters
-- Every put the GIN kernel makes reached the capture, exactly once.
--
-- The workload puts `puts_per_iter` chunks of `chunk_bytes` to its right
-- neighbour once per iteration, from a device kernel through ncclGin, each put
-- carrying a signal increment. Under GDAKI the GPU builds every WQE in a
-- GPU-resident send queue and a host thread rings the doorbell, so the door that
-- sees them is the host doorbell (providers/mlx5dv/host_doorbell.hpp) and the
-- rows carry submitter_name = 'host_doorbell'.
--
-- Two claims, because GIN's put is two WQEs and both have to be there:
--   * the payload: chunk_bytes x puts_per_iter x iters of rdma_write per rank,
--     stated in BYTES so it holds however the transport splits or coalesces;
--   * the signal: one atomic per put, which is what makes the neighbour's
--     waitSignal return, and what a decoder that lost the tail of a batch would
--     be missing.
--
-- Measured on MN5 job 45913525 (2 nodes, 1 rank each, NCCL 2.29.3, GDAKI with
-- NCCL_GIN_GDAKI_NIC_HANDLER=1): per rank 20,480 rdma_write of 128 B =
-- 2,621,440 B and 20,480 atomic_fa of 8 B, wqes_lapped=0 — the workload's own
-- numbers, exactly, on both ranks and in auto mode too.
--
-- `requires: mlx5_devx_qp`, never mlx5_device_send: a run whose door saw nothing
-- must FAIL here, and requiring the table it would have filled would turn
-- exactly that into a [skip]. This is case 12's data_plane_puts_arrived.sql
-- applied to NCCL instead of NVSHMEM, and it exists for the same reason: case 12
-- passed for three weeks on a capture with no workload traffic in it.
WITH payload AS (
    SELECT CAST(rank AS INTEGER) AS rank, sum(CAST(length AS INTEGER)) AS bytes
    FROM mlx5_device_send
    WHERE submitter_name = 'host_doorbell' AND opcode_name = 'rdma_write'
    GROUP BY CAST(rank AS INTEGER)
),
signals AS (
    SELECT CAST(rank AS INTEGER) AS rank, count(*) AS n
    FROM mlx5_device_send
    WHERE submitter_name = 'host_doorbell' AND opcode_name LIKE 'atomic%'
    GROUP BY CAST(rank AS INTEGER)
)
SELECT r.rank,
       coalesce(p.bytes, 0) AS payload_bytes,
       (SELECT chunk_bytes * puts_per_iter * iters FROM params) AS payload_expected,
       coalesce(s.n, 0)     AS signals,
       (SELECT puts_per_iter * iters FROM params)                AS signals_expected
FROM ranks_seen r
LEFT JOIN payload p ON p.rank = r.rank
LEFT JOIN signals s ON s.rank = r.rank
WHERE coalesce(p.bytes, 0) != (SELECT chunk_bytes * puts_per_iter * iters FROM params)
   OR coalesce(s.n, 0) != (SELECT puts_per_iter * iters FROM params)

UNION ALL

SELECT -1, (SELECT count(*) FROM ranks_seen), (SELECT ranks FROM params), 0, 0
WHERE (SELECT count(*) FROM ranks_seen) != (SELECT ranks FROM params);
