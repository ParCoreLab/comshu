-- requires: ucx_tag_send params.fft_nr3px params.fft_ncpx
-- The size of one transpose chunk follows from the FFT grid, and it must be in
-- the capture to the byte.
--
-- FFTXlib/src/fft_scatter.f90:662-664 builds each peer's chunk as
--
--     nr3px    = MAXVAL( desc%nr3p )      ! Z planes per process
--     ncpx     = MAXVAL( ncp_ )           ! Z columns per process
--     sendsize = ncpx * nr3px * (me2_end - me2_start + 1)
--
-- of MPI_DOUBLE_COMPLEX, i.e. 16 B each, and the last factor is 1 at nyfft = 1
-- (which is forced here: -ntg > 1 is a hard errore under __CUDA, see
-- Modules/mp_global.f90:83-84). Both maxima are a balanced split over
-- P = nproc_bgrp = ranks/npool (mp_pools.f90:73, mp_bands.f90:90), so
--
--     transpose_bytes = (nr3 / P) * ceil(sticks_dense_total / P) * 16
--
-- nr3 = 288 and sticks_dense_total = 11837 are pw.x's OWN printed numbers for
-- AUSURF112 and are INDEPENDENT of the rank division -- round 1 verified they
-- print identically at two divisions, because they are fixed by the cell and by
-- ecutrho, not by the run.
--
-- fft_nr3px and fft_ncpx are the two factors evaluated at this arm's P, written
-- into meta-r2.json BEFORE the run (36 and 1480 at P=8; 72 and 2960 at P=4).
-- They are carried as params rather than divided here because integer division
-- means different things on sqlite3 and DuckDB, and an oracle that changes value
-- with the SQL engine is not an oracle. The arithmetic is auditable: the job
-- script prints pw.x's own `sticks: dense Max` line, which IS MAXVAL(ncp_).
--
-- WHY THIS IS AN ORACLE AND NOT A GOLDEN. The npool=2 arm predicts 3 409 920 B
-- at 8 ranks. Round 1 MEASURED exactly 3 409 920 B at 4 ranks with npool=1 --
-- a different rank count and a different flag -- because the formula depends on
-- nproc_bgrp and on nothing else. A fit would not survive that.
--
-- WHAT IT CATCHES: a `count` that is not bytes for this datatype; a transpose
-- that never reached ucp_tag_send (hcoll intercepting the all-to-all -- the job
-- disables it, and this is what notices if the disable did not take); a
-- decomposition that is not what the flags asked for.
--
-- WRITTEN FOR BOTH RENDERINGS. Whether the CSV export prints a uint64 count in
-- decimal or in hex is not documented (QUESTIONS.md), so the comparison is made
-- against the decimal integer AND against the hex spelling of the same number.
SELECT 'no tagged send carries the transpose chunk size the FFT grid implies' AS problem,
       (SELECT fft_nr3px FROM params)                            AS nr3px,
       (SELECT fft_ncpx  FROM params)                            AS ncpx,
       (SELECT fft_nr3px * fft_ncpx * 16 FROM params)            AS predicted_bytes,
       (SELECT count(*) FROM ucx_tag_send)                       AS tagged_sends_seen
WHERE NOT EXISTS (
        SELECT 1 FROM ucx_tag_send
        WHERE CAST(count AS INTEGER) = (SELECT fft_nr3px * fft_ncpx * 16 FROM params)
           OR lower(replace(CAST(count AS TEXT), '0x', ''))
              = lower(printf('%x', (SELECT fft_nr3px * fft_ncpx * 16 FROM params)))
      );
