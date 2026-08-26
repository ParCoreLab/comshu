-- requires: comm_edges params.ranks
-- With one pool and one band group, the FFT all-to-all spans MPI_COMM_WORLD, so
-- the rank-space matrix must be COMPLETE: every ordered pair carries traffic.
--
-- THE DERIVATION, entirely from QE's source and this run's flags. No capture is
-- consulted to produce the expected answer:
--
--   Modules/mp_pools.f90:73   nproc_pool = parent_nproc / npool  = ranks / 1
--   Modules/mp_bands.f90:90   nproc_bgrp = parent_nproc / nbgrp  = ranks / 1
--   so intra_bgrp_comm is all `ranks` ranks, and intra_bgrp_comm is the
--   communicator the FFT descriptor is built on.
--   FFTXlib/src/fft_scatter.f90:715, :792   mpi_alltoall(..., desc%comm3)
--   FFTXlib/src/fft_scatter.f90:433, :531   mpi_alltoall(..., desc%comm2)
--   comm2 and comm3 are the row and column sub-communicators of that same set;
--   over one SCF iteration their product covers it, so every ordered pair of
--   distinct ranks exchanges plane data at least once.
--
-- This is the claim no benchmark in tests/hpc/cases can make. A ring or a star
-- has a sparse matrix by construction; a plane-wave FFT transpose has a DENSE
-- one by construction, and "dense" is checkable without knowing a single count.
--
-- Volume is deliberately NOT asserted. How many all-to-alls run depends on how
-- many Davidson steps the diagonaliser takes, which is data-dependent and is not
-- ours to freeze — asserting it would be a golden wearing an oracle's clothes.
--
-- Names no rank: the rank set is generated from params.ranks and the output is
-- the pairs that are MISSING. The dst_rank = -1 guard is not decoration —
-- link_sql.hpp keeps unresolved peers as a row rather than dropping them
-- precisely so a matrix cannot lie about its totals, and counting those rows as
-- edges would let unattributed traffic satisfy a completeness claim.
WITH RECURSIVE r(n) AS (
    SELECT 0 UNION ALL SELECT n + 1 FROM r WHERE n + 1 < (SELECT ranks FROM params)
),
edge AS (
    SELECT DISTINCT CAST(src_rank AS INTEGER) AS src, CAST(dst_rank AS INTEGER) AS dst
    FROM comm_edges
    WHERE CAST(dst_rank AS INTEGER) >= 0 AND CAST(sends AS INTEGER) > 0
)
SELECT 'ordered pair carries no traffic, but the FFT all-to-all spans it' AS problem,
       a.n AS src, b.n AS dst
FROM r a, r b
WHERE a.n <> b.n
  AND NOT EXISTS (SELECT 1 FROM edge e WHERE e.src = a.n AND e.dst = b.n)
ORDER BY src, dst;
