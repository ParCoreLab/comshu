-- requires: ucx_tag_send params.ranks params.npool
-- Each rank's heavy tagged-send traffic must go to exactly (ranks/npool - 1)
-- peers — the size of its pool, minus itself.
--
-- WRITTEN BEFORE THE CAPTURE EXISTED, and derived only from QE's source and this
-- run's flags:
--
--   Modules/mp_pools.f90:73   nproc_pool = parent_nproc / npool
--   Modules/mp_pools.f90:79   my_pool_id = parent_mype  / nproc_pool
--   Modules/mp_pools.f90:86   mp_comm_split(...) -> intra_pool_comm, so pools
--                             are CONTIGUOUS blocks of nproc_pool ranks
--   Modules/mp_bands.f90:90   nproc_bgrp = nproc_pool when nbgrp = 1, and
--                             intra_bgrp_comm is the communicator the FFT
--                             descriptor is built on
--   FFTXlib/src/fft_scatter.f90:715, :792   mpi_alltoall(..., desc%comm3)
--   FFTXlib/src/fft_scatter.f90:433, :531   mpi_alltoall(..., desc%comm2)
--
-- The Davidson loop drives thousands of those all-to-alls per SCF iteration and
-- they cannot leave the pool. Everything that crosses a pool boundary is a
-- handful of per-iteration reductions over inter_pool_comm (mp_pools.f90:92;
-- PW/src/sum_band.f90 sums rho once per iteration). So a rank's tagged sends
-- concentrate on its nproc_pool - 1 pool-mates, and the prediction is arithmetic:
--
--   ranks=4, npool=1 -> nproc_pool=4 -> 3 heavy endpoints per rank
--   ranks=4, npool=2 -> nproc_pool=2 -> 1 heavy endpoint per rank
--
-- WHY ENDPOINTS AND NOT RANKS. Because the peer rank is not recoverable from
-- this capture at all — ucx_worker_address is never written on an OpenMPI
-- point-to-point run (see ucx_peer_matrix_is_recoverable.sql, which states that
-- defect). This assertion is deliberately built to need only the LOCAL side: a
-- count of distinct ucp_ep handles and how the sends divide among them. It names
-- no rank, no endpoint handle and no peer.
--
-- WHY A 10 % THRESHOLD. "Heavy" needs a cut, and it is set an order of magnitude
-- below the even share the decomposition predicts (1/3 at npool=1) and an order
-- of magnitude above what a once-per-SCF-iteration reduction can contribute
-- against thousands of FFT transposes. It is a threshold, not a fitted constant:
-- it was chosen from the ratio of message COUNTS the source implies, before any
-- capture was read. If it turns out to sit inside the real distribution, that is
-- reported as a finding, not tuned away.
WITH total AS (
    SELECT CAST(rank AS INTEGER) AS rank, count(*) AS n
    FROM ucx_tag_send GROUP BY 1
),
per_ep AS (
    SELECT CAST(rank AS INTEGER) AS rank, endpoint, count(*) AS n
    FROM ucx_tag_send GROUP BY 1, 2
),
heavy AS (
    SELECT t.rank, sum(CASE WHEN 1.0 * e.n / t.n >= 0.10 THEN 1 ELSE 0 END) AS heavy_eps
    FROM total t JOIN per_ep e ON e.rank = t.rank
    GROUP BY t.rank
)
SELECT 'rank''s heavy-endpoint count does not match its pool size' AS problem,
       rank, heavy_eps,
       (SELECT ranks FROM params) / (SELECT npool FROM params) - 1 AS expected
FROM heavy
WHERE heavy_eps <> (SELECT ranks FROM params) / (SELECT npool FROM params) - 1
ORDER BY rank;
