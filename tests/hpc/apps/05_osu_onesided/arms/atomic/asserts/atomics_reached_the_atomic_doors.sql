-- requires: ucx_atomic params.atomic_ops params.atomic_bytes
-- The workload's own atomics reached the atomic doors, and the fabric agreed to
-- them.
--
-- THE DOORS THIS IS HERE TO WITNESS. UCX's three atomic doors were hooked on a
-- static ELF survey of MN5's HPC-X and, until case 22, never observed firing;
-- case 22 is a workload this project wrote for the purpose. osu_acc_latency and
-- osu_fop_latency are the same doors reached by code nobody here has ever
-- edited, with a count that comes from `-i 20 -x 5` rather than from a probe
-- written to make the assertion true.
--
-- 25 = warmup + iterations, one operation per iteration: MPI_Accumulate in
-- run_acc_with_flush, MPI_Fetch_and_op in run_fop_with_flush, both of exactly
-- one MPI_INT. That is why one assertion serves both benchmarks.
--
-- A FLOOR ON THE COUNT AND A CEILING ON NOTHING, because OpenMPI's own window
-- locking issues 64-bit fetch-and-add and compare-and-swap on its state window
-- and those are legitimately in this table too. The workload's operations are
-- separated from MPI's by OPERAND WIDTH — 4 bytes, from `-T MPI_INT` on the
-- command line — which is the one discriminator that comes from the workload
-- rather than from a guess about OpenMPI's internals.
--
-- SUCCEEDING, and this is the clause that matters. Case 22's job 45177421 held
-- 80 atomic rows of which every one of the workload's twenty had failed with
-- status -5, because they asked for a one-byte atomic; a check for a non-empty
-- table would have gone green over a workload whose every atomic was refused.
-- UCS_OK is 0 and UCS_INPROGRESS is 1, so `status >= 0` is the agreement and
-- anything negative is a refusal. The raw integer is used rather than
-- status_class because the class vocabulary is published nowhere this study may
-- read and the two export engines do not decode it the same way.
--
-- IF THIS FAILS WITH ZERO 4-BYTE ATOMICS, read PREDICTION.md before reading it
-- as a comshu defect: the alternative reading — osc/ucx taking its generic
-- lock-get-put path — is pre-registered there, and the discriminator is whether
-- a matching population of 4-byte gets and puts appears in ucx_rma instead.
SELECT 'the workload issued atomics that the transport refused' AS problem,
       rank,
       CAST(op_size AS INTEGER) AS op_size,
       CAST(status AS INTEGER)  AS status,
       count(*)                 AS n
FROM ucx_atomic
WHERE CAST(status AS INTEGER) < 0
GROUP BY rank, CAST(op_size AS INTEGER), CAST(status AS INTEGER)
UNION ALL
SELECT 'fewer successful atomics of the workload''s operand width than the command line asked for',
       NULL,
       (SELECT atomic_bytes FROM params),
       NULL,
       (SELECT count(*) FROM ucx_atomic
        WHERE CAST(op_size AS INTEGER) = (SELECT atomic_bytes FROM params)
          AND CAST(status AS INTEGER) >= 0)
WHERE (SELECT count(*) FROM ucx_atomic
       WHERE CAST(op_size AS INTEGER) = (SELECT atomic_bytes FROM params)
         AND CAST(status AS INTEGER) >= 0)
      < (SELECT atomic_ops FROM params);
