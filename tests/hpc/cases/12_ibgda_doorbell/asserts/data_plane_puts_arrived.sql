-- requires: mlx5_devx_qp params.ranks params.elems params.iters params.elem_bytes
-- Every put the workload makes reached the capture, exactly once — the one claim
-- this case exists for, and for three weeks the one it did not check.
--
-- MN5 boards 45497122 through 45887330 decoded 63 sends per run, and every one
-- of them PASSED: 30 atomics per rank and three 128 B writes on rank 0, all of
-- them NVSHMEM's own, and not one of the 40,960 puts. The workload binary was
-- sm_80-only on an H100, its kernel never ran, and it printed "OK" regardless
-- (fixed in d45b536). Every other assertion here held, because each one checks
-- the rows that exist: they were named, sized, in sequence and started at the
-- queue head. The wire check held, because it is an upper bound. The
-- application's output matched the control's, because the control's kernel did
-- not run either. Nothing tied the capture's volume to the workload's.
--
-- The put-ring writes the same `elems` ints to its neighbour once per
-- iteration. So the invariant is on the DESTINATION, not on a send count: every
-- byte of that buffer is written exactly `iters` times. It holds whatever
-- NVSHMEM coalesces — over InfiniBand a full warp's contiguous puts become one
-- 128 B write (support_half_av_seg), so MN5 board 45895420 shows 32 blocks x 20
-- per rank; over RoCE each put is its own 4 B write, 1,024 blocks x 20 — and
-- both cover elems x elem_bytes = 4,096 B. A block decoded twice is written
-- iters + 1 times, a block lost is written fewer, and either drops its bytes
-- out of the sum.
--
-- NVSHMEM's own three writes each go to a distinct address once, so they
-- cannot pass for the workload unless iters = 1. This case runs 20.
--
-- `requires: mlx5_devx_qp`, never mlx5_device_send: a run that decoded nothing
-- must FAIL here, and requiring the table it would have filled would turn
-- exactly that into a [skip]. A rank with no puts at all is caught by the left
-- join; a rank missing from the capture entirely, by the second branch.
-- Case 11's data_plane_puts_arrived.sql is the same claim for ibdevx.
--
-- Case 25 runs this same file (a symlink) against its RoCE leg. Its own
-- leg-to-leg comparisons are relative — rc against dc, rc against roce_dc — so
-- a workload that sent nothing agrees with itself in every leg, which is
-- exactly how case 25's histogram check spent its first life comparing three
-- barrier writes.
WITH blocks AS (
    SELECT CAST(rank AS INTEGER)   AS rank,
           raddr,
           CAST(length AS INTEGER) AS len,
           count(*)                AS times
    FROM mlx5_device_send
    WHERE opcode_name = 'rdma_write'
    GROUP BY CAST(rank AS INTEGER), raddr, CAST(length AS INTEGER)
),
ring AS (
    SELECT rank, sum(len) AS bytes
    FROM blocks
    WHERE times = (SELECT iters FROM params)
    GROUP BY rank
)
SELECT r.rank,
       coalesce(g.bytes, 0)                   AS bytes_written_iters_times,
       (SELECT elems * elem_bytes FROM params) AS expected
FROM ranks_seen r
LEFT JOIN ring g ON g.rank = r.rank
WHERE coalesce(g.bytes, 0) != (SELECT elems * elem_bytes FROM params)

UNION ALL

SELECT -1, (SELECT count(*) FROM ranks_seen), (SELECT ranks FROM params)
WHERE (SELECT count(*) FROM ranks_seen) != (SELECT ranks FROM params);
