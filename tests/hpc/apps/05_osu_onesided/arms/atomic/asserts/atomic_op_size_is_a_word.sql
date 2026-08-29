-- requires: ucx_atomic
-- Every recorded atomic operand is a machine word the fabric can actually
-- operate on.
--
-- UcxAtomic.op_size "means it when it says 4 or 8" — that is the record's own
-- claim, and UCX's atomic API supports 32- and 64-bit operands and nothing
-- else. So on THIS arm, where every operation the workload issues is a 4-byte
-- MPI_INT and every operation OpenMPI issues for its own locking is an 8-byte
-- word, any other value in the column is a value nobody could have asked for.
--
-- WHAT IT CATCHES. The atomic doors have three separate opcode spaces which
-- number the same operations differently, and the record normalises across
-- them; a normalisation that reads the size from the wrong argument slot yields
-- a plausible small integer rather than a crash. 1, 2, 16 or 0 here is that
-- mistake, and no count, byte total or key join in this directory would see it:
-- the row would still resolve, still succeed, still be on the right rank.
--
-- SCOPED TO THIS ARM ON PURPOSE. It would be WRONG as a global assertion: the
-- contrast arm in job.sbatch runs osu_fop_latency at its default MPI_CHAR and
-- is expected to produce op_size = 1 rows, which is the knob doing its job.
-- That arm is not judged by this directory, and PREDICTION.md says so before
-- either arm ran.
SELECT 'an atomic operand width is not a word UCX can operate on' AS problem,
       rank,
       CAST(op_size AS INTEGER) AS op_size,
       count(*)                 AS n
FROM ucx_atomic
WHERE CAST(op_size AS INTEGER) NOT IN (4, 8)
GROUP BY rank, CAST(op_size AS INTEGER);
