-- requires: ucx_tag_send params.ranks params.npool
-- Each rank's tagged-send BYTES must concentrate on exactly (ranks/npool - 1)
-- peers — the size of its pool, minus itself.
--
-- THIS FILE REPLACES A CLAIM THAT WAS WRONG, and the original is kept beside it
-- (endpoint_fanout_matches_pool_size.sql, marked expect_fail) rather than
-- deleted, because the reason it failed is a real property of the stack and is
-- worth keeping visible.
--
-- WHAT WAS WRONG. The original counted MESSAGES. It passed at npool=1 (3 heavy
-- endpoints, 35 %/35 %/30 %) and failed at npool=2, where every rank showed 2
-- heavy endpoints instead of 1. The capture says why, and it is not a threshold
-- artefact: rank 0's second endpoint carried 2,638 sends whose most frequent
-- lengths are 0, 4, 8 and 1,024 bytes, against 7,340 sends of 4,921,600 bytes on
-- the first. The second endpoint is heavy in COUNT and negligible in BYTES —
-- it is control and synchronisation traffic, not data.
--
-- So the original assertion encoded an unstated assumption — that message count
-- is a proxy for data movement — which this application falsifies. The
-- independent reason for preferring bytes is already written down in this
-- repository: ucx_records.hpp:80-90 says a UCP count "must never be added to a
-- bytes total" precisely because the two quantities answer different questions,
-- and 19_ucx_mpi's workload_note refuses to assert counts at all for the same
-- reason. The fix is to measure the quantity the decomposition actually
-- predicts, not to move the threshold until the old measure agrees.
--
-- THE CLAIM, unchanged in substance and derived the same way:
--   mp_pools.f90:73,79,86  pool(r) = r / (ranks/npool), pools are contiguous
--   mp_bands.f90:90        intra_bgrp_comm = intra_pool_comm when nbgrp = 1
--   fft_scatter.f90:715    the all-to-all runs on desc%comm3, inside the pool
-- so a rank's FFT transpose bytes go to its nproc_pool - 1 pool-mates and
-- nowhere else. The 10 % cut is an order of magnitude below the even share the
-- decomposition predicts and orders of magnitude above the control traffic.
WITH total AS (
    SELECT CAST(rank AS INTEGER) AS rank, sum(CAST(count AS INTEGER)) AS b
    FROM ucx_tag_send GROUP BY 1
),
per_ep AS (
    SELECT CAST(rank AS INTEGER) AS rank, endpoint, sum(CAST(count AS INTEGER)) AS b
    FROM ucx_tag_send GROUP BY 1, 2
),
heavy AS (
    SELECT t.rank, sum(CASE WHEN 1.0 * e.b / t.b >= 0.10 THEN 1 ELSE 0 END) AS heavy_eps
    FROM total t JOIN per_ep e ON e.rank = t.rank
    GROUP BY t.rank
)
SELECT 'rank''s heavy-by-bytes endpoint count does not match its pool size' AS problem,
       rank, heavy_eps,
       (SELECT ranks FROM params) / (SELECT npool FROM params) - 1 AS expected
FROM heavy
WHERE heavy_eps <> (SELECT ranks FROM params) / (SELECT npool FROM params) - 1
ORDER BY rank;
