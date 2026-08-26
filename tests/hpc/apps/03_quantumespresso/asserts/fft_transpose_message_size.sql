-- requires: ucx_tag_send params.ranks params.npool params.fft_dense_nr3 params.fft_dense_sticks_total
-- The size of the FFT transpose message, computed from QE's own parallelization
-- report BEFORE any communication happens, must appear in the capture.
--
-- This is the strongest claim in this directory: a number the application itself
-- determines, derived from its own log and its own source, that the capture must
-- reproduce exactly — not approximately, not in aggregate, but as a message
-- length that is actually present.
--
-- THE DERIVATION.
--   pw.x prints, before the SCF loop starts:
--       sticks:   dense  smooth     PW
--       Sum       11837    5915    1537          <- fft_dense_sticks_total
--       Dense  grid: ... FFT dimensions: ( 180,  90, 288)   <- fft_dense_nr3
--       R & G space division:  proc/nbgrp/npool/nimage = <nproc_rg>
--   and Modules/mp_pools.f90:73 + Modules/mp_bands.f90:90 fix
--       nproc_rg = nproc_bgrp = nproc_pool = ranks / npool.
--
--   FFTXlib/src/fft_scatter.f90 builds the all-to-all send buffer as
--   nr3px * ncpx complex numbers per peer: nr3px is this rank's share of the
--   nr3 planes and ncpx is the largest number of sticks any rank owns. Both
--   follow from nproc_rg by even division with the remainder rounded up:
--       nr3px = fft_dense_nr3        / nproc_rg
--       ncpx  = ceil(sticks_total    / nproc_rg)
--   and the datatype is MPI_DOUBLE_COMPLEX (fft_scatter.f90:173, :715, :792),
--   which is 16 bytes.
--
--   So   bytes = (fft_dense_nr3 / nproc_rg) * ceil(sticks_total / nproc_rg) * 16
--
--   ranks=4, npool=1 -> nproc_rg=4 -> (288/4) * 2960 * 16 =  3,409,920 B
--   ranks=4, npool=2 -> nproc_rg=2 -> (288/2) * 5919 * 16 = 13,637,376 B
--
-- WHY THIS IS AN ORACLE AND NOT A GOLDEN. Neither input is read off a capture.
-- fft_dense_nr3 and fft_dense_sticks_total are fixed by the input file's cell
-- and ecutrho — pw.x prints the same 288 and the same 11837 at BOTH rank
-- divisions, which is exactly why they can be carried in `workload` as
-- constants while nproc_rg comes from params. The quantity that changes between
-- the two captures is derived, not measured. It names no rank and no endpoint.
--
-- WHY count IS BYTES HERE, stated because ucx_records.hpp warns that it is not
-- universally so: OpenMPI's pml_ucx sends contiguous buffers with a contiguous
-- UCP datatype, for which the UCP count IS the byte count. Cross-checked
-- independently — job 45082487's rc_verbs failure printed
-- "ibv_reg_mr(address=..., length=3409920, ...) failed" for the same buffer this
-- capture records as count=3409920.
SELECT 'the FFT transpose message size QE''s own report implies is absent from the capture'
         AS problem,
       (SELECT fft_dense_nr3 FROM params)
         / ((SELECT ranks FROM params) / (SELECT npool FROM params))
       * (((SELECT fft_dense_sticks_total FROM params)
            + (SELECT ranks FROM params) / (SELECT npool FROM params) - 1)
          / ((SELECT ranks FROM params) / (SELECT npool FROM params)))
       * 16 AS expected_bytes,
       (SELECT count(*) FROM ucx_tag_send) AS sends_in_capture
WHERE NOT EXISTS (
    SELECT 1 FROM ucx_tag_send
    WHERE CAST(count AS INTEGER) =
          (SELECT fft_dense_nr3 FROM params)
            / ((SELECT ranks FROM params) / (SELECT npool FROM params))
          * (((SELECT fft_dense_sticks_total FROM params)
               + (SELECT ranks FROM params) / (SELECT npool FROM params) - 1)
             / ((SELECT ranks FROM params) / (SELECT npool FROM params)))
          * 16);
