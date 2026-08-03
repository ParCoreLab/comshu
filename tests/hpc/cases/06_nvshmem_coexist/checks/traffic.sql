-- NVLink side: device-side nvshmem_int_p peer stores, kernel-filtered to
-- put_to_next. Identical oracle to case 04 (same 4 PEs / elems / iters) — and
-- that is the point: the second provider must not perturb the first. Any drift
-- from case 04's numbers here means gpumem's capture changed when ib was
-- loaded alongside it. It dropped its `load` rows on 2026-08-03 for the same
-- reason case 04's did — see that file; the two must stay identical.
SELECT rank, op, count(*) AS n, sum(CAST(size AS INTEGER)) AS bytes
FROM gpu_access
GROUP BY rank, op
ORDER BY CAST(rank AS INTEGER), op;
