-- requires: comm_edges params.ranks params.npool
-- With npool > 1 the FFT all-to-all is confined INSIDE a pool, so every rank
-- must send more messages to its own pool than to the rest of the world.
--
-- THE DERIVATION — arithmetic, from Modules/mp_pools.f90 and nothing else:
--   :73  nproc_pool = parent_nproc / npool
--   :79  my_pool_id = parent_mype  / nproc_pool
--   :86  mp_comm_split(parent_comm, my_pool_id, parent_mype, intra_pool_comm)
-- so pool(r) = r / (ranks / npool), integer division, and pools are CONTIGUOUS
-- blocks of ranks. mp_bands.f90:90 makes intra_bgrp_comm a subset of that
-- (nbgrp = 1 here, so equal to it), and intra_bgrp_comm is the communicator the
-- FFT descriptor is built on — fft_scatter.f90:715 alltoalls over desc%comm3,
-- which cannot leave the pool.
--
-- What legitimately crosses a pool boundary is only the SCF-level reduction over
-- inter_pool_comm (mp_pools.f90:92; PW/src/sum_band.f90 sums rho over pools once
-- per iteration, PW/src/electrons.f90 the eigenvalues) — a handful of
-- collectives per SCF step against the thousands of all-to-alls the Davidson
-- loop issues inside the pool.
--
-- So "more inside than outside" is a FLOOR, not a tight bound, and it is stated
-- as a floor on purpose: the tight ratio would be a golden, because it depends
-- on the Davidson step count, which is data-dependent.
--
-- This is the assertion that makes -npool visible in the trace rather than
-- merely asserted by QE's documentation. It names no rank: the partition is a
-- pure function of params.ranks and params.npool, and the output is the ranks
-- that violate it.
WITH edge AS (
    SELECT CAST(src_rank AS INTEGER) AS src, CAST(dst_rank AS INTEGER) AS dst,
           CAST(sends AS INTEGER) AS sends
    FROM comm_edges WHERE CAST(dst_rank AS INTEGER) >= 0
),
sized AS (SELECT (SELECT ranks FROM params) / (SELECT npool FROM params) AS nproc_pool),
split AS (
    SELECT src,
           sum(CASE WHEN src / (SELECT nproc_pool FROM sized)
                     = dst / (SELECT nproc_pool FROM sized) AND src <> dst
                    THEN sends ELSE 0 END) AS in_pool,
           sum(CASE WHEN src / (SELECT nproc_pool FROM sized)
                    <> dst / (SELECT nproc_pool FROM sized)
                    THEN sends ELSE 0 END) AS out_of_pool
    FROM edge GROUP BY src
)
SELECT 'rank sends no more inside its pool than outside it' AS problem,
       src AS rank, in_pool, out_of_pool
FROM split
WHERE in_pool <= out_of_pool
ORDER BY rank;
