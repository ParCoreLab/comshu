-- requires: endpoint mlx5_devx_qp params.ranks params.elems params.iters params.elem_bytes params.app_buffer_bytes
-- Every byte the GPU put into the ring lands in the PEER's app buffer in
-- object_access: the buffer the workload allocated, on the rank it targeted,
-- iters times over.
--
-- data_plane_puts_arrived.sql proves the puts are in the capture. This proves
-- the derived layer can say WHERE they went, which it could not until
-- 2026-11-09: object_access had no branch for mlx5_device_send, so on MN5 job
-- 45925495 comm_edges counted all 1,347 decoded sends exactly while the Objects
-- panel showed none of them. Re-linked with the devx branch, the same capture
-- placed 640 puts = 81,920 B per rank in the peer's 4,096 B buffer at offsets
-- 0 … 3,968, and NVSHMEM's own signal traffic in its 512 MB symmetric heap.
--
-- The buffer is found by its SIZE (app_buffer_bytes), never by address or id:
-- both are allocation-dependent, and an id is a per-rank intern handle anyway.
-- The heap is orders of magnitude larger, so the size names one object.
--
-- WHY `endpoint` IS REQUIRED, though nothing below reads it. object_access is a
-- derived table, so this assertion can only be answered by the Parquet export.
-- Naming a table the CSV export lacks is what routes it there (sql_check.sh,
-- pq_only). It must not be object_access itself: `requires` skips on an EMPTY
-- table, and an empty object_access is precisely the failure this exists to
-- catch. endpoint is non-empty on any capture whose queues connected.
WITH placed AS (
    SELECT a.actor_rank, a.owner_rank, sum(a.size) AS bytes,
           min(a.obj_offset) AS lo, max(a.obj_offset + a.size) AS hi
    FROM object_access a
    JOIN (SELECT DISTINCT rank, id, range_size FROM mem_object) m
      ON m.rank = a.owner_rank AND m.id = a.object
    WHERE a.source = 'devx'
      AND m.range_size = (SELECT app_buffer_bytes FROM params)
    GROUP BY a.actor_rank, a.owner_rank
)
SELECT r.rank                                          AS actor_rank,
       p.owner_rank,
       coalesce(p.bytes, 0)                            AS bytes_in_app_buffer,
       (SELECT iters * elems * elem_bytes FROM params) AS expected,
       p.lo, p.hi
FROM ranks_seen r
LEFT JOIN placed p ON p.actor_rank = r.rank
WHERE coalesce(p.bytes, 0) != (SELECT iters * elems * elem_bytes FROM params)
   OR p.owner_rank = r.rank                     -- a ring put never targets itself
   OR p.lo != 0
   OR p.hi != (SELECT app_buffer_bytes FROM params)

UNION ALL

SELECT -1, NULL, (SELECT count(*) FROM ranks_seen), (SELECT ranks FROM params), NULL, NULL
WHERE (SELECT count(*) FROM ranks_seen) != (SELECT ranks FROM params);
