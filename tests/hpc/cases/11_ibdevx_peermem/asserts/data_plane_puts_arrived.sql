-- requires: mlx5_devx_qp params.ranks params.elems params.iters params.elem_bytes
-- Every put the workload makes reached the capture, exactly once.
--
-- The put-ring does `elems` nvshmem_int_p per iteration to its neighbour. Under
-- ibdevx each is one RDMA_WRITE of elem_bytes, built in a host send queue by
-- NVSHMEM's CPU proxy — which is precisely the data plane no door saw until the
-- send-queue watcher (providers/mlx5dv/sq_watch.hpp). So each rank must carry
-- elems x iters of them: 2,048 x 4 B for this case's `1024 2`, measured on du04
-- before it was asserted (plans/PLAN-20260914-1750, step 1).
--
-- Counted by length = elem_bytes, not all rdma_writes: rank 0 also writes three
-- 128 B blocks that are NVSHMEM's own, not the workload's, and an assertion that
-- counted them would be encoding today's transport rather than the workload.
--
-- `requires: mlx5_devx_qp`, never mlx5_device_send: a run whose watcher saw
-- nothing must FAIL here, and requiring the table it would have filled would
-- turn exactly that into a [skip]. A rank with no puts at all is caught by the
-- left join; a rank missing from the capture entirely, by the second branch.
WITH puts AS (
    SELECT CAST(rank AS INTEGER) AS rank, count(*) AS n
    FROM mlx5_device_send
    WHERE submitter_name = 'sq_watch'
      AND opcode_name = 'rdma_write'
      AND CAST(length AS INTEGER) = (SELECT elem_bytes FROM params)
    GROUP BY CAST(rank AS INTEGER)
)
SELECT r.rank,
       coalesce(p.n, 0)                     AS puts,
       (SELECT elems * iters FROM params)   AS expected
FROM ranks_seen r
LEFT JOIN puts p ON p.rank = r.rank
WHERE coalesce(p.n, 0) != (SELECT elems * iters FROM params)

UNION ALL

SELECT -1, (SELECT count(*) FROM ranks_seen), (SELECT ranks FROM params)
WHERE (SELECT count(*) FROM ranks_seen) != (SELECT ranks FROM params);
