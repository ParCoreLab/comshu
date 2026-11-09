-- requires: endpoint mlx5_devx_qp params.ranks params.chunk_bytes params.puts_per_iter params.iters params.app_buffer_bytes
-- Every GIN put lands in the PEER's window in object_access, at offsets inside
-- the window the workload registered, and nothing GIN sent is left unplaced.
--
-- gin_puts_arrived.sql proves the puts are in the capture. This proves the
-- derived layer can say WHERE they went, which took two fixes. GIN registers its
-- window zero-based (iova 0), so a put names an OFFSET: the reader translates
-- through ib_mem_region_iova (29c81e7). And the window's VA comes only from
-- NCCL's cuMemGetHandleForAddressRange, which statically linked cudart reaches
-- through the driver export table (bb41c67, 0eed85a). Before both, MN5 job
-- 45925495 placed 0 of 81,920. MN5 job 48812008 re-linked: per rank 20,480 puts
-- = 2,621,440 B at window offsets 0 … 131,072, the 20,480 signals in NCCL's
-- signal buffer, and unplaced_offset_sends = 0.
--
-- The payload is told from the signals by SIZE (chunk_bytes vs an 8 B atomic),
-- and the window by its extent (app_buffer_bytes). The object itself is the
-- ncclMemAlloc allocation, rounded up to the VMM granularity (2 MiB here), so
-- its size is the allocator's, not the workload's, and is not asserted.
--
-- WHY `endpoint`: object_access is derived, so naming a table the CSV export
-- lacks routes this to the Parquet path (sql_check.sh, pq_only), and it must
-- not be object_access, whose emptiness is the failure being caught. See case
-- 12's gpu_puts_land_in_the_app_buffer.sql.
WITH placed AS (
    SELECT actor_rank, owner_rank, sum(size) AS bytes,
           min(obj_offset) AS lo, max(obj_offset + size) AS hi
    FROM object_access
    WHERE source = 'devx' AND size = (SELECT chunk_bytes FROM params)
    GROUP BY actor_rank, owner_rank
)
SELECT r.rank                                                    AS actor_rank,
       p.owner_rank,
       coalesce(p.bytes, 0)                                      AS payload_placed,
       (SELECT chunk_bytes * puts_per_iter * iters FROM params)  AS expected,
       p.lo, p.hi
FROM ranks_seen r
LEFT JOIN placed p ON p.actor_rank = r.rank
WHERE coalesce(p.bytes, 0) != (SELECT chunk_bytes * puts_per_iter * iters FROM params)
   OR p.owner_rank = r.rank
   OR p.lo != 0
   OR p.hi != (SELECT app_buffer_bytes FROM params)

UNION ALL

SELECT -1, NULL, n, 0, NULL, NULL
FROM link_health
WHERE finding = 'unplaced_offset_sends' AND n != 0

UNION ALL

SELECT -2, NULL, (SELECT count(*) FROM ranks_seen), (SELECT ranks FROM params), NULL, NULL
WHERE (SELECT count(*) FROM ranks_seen) != (SELECT ranks FROM params);
