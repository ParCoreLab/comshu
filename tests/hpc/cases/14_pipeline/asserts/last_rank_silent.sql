-- requires: gpu_access params.ranks
-- The last PE in the chain sends nothing, so it must appear as a source in no
-- row at all.
--
-- "Absent" and "zero" are different findings and only one of them is checkable
-- here: a rank that sent nothing produces no gpu_access rows, so this asserts
-- the absence directly. It is the structural counterpart to the volume check —
-- a stage that dropped its send shows up as a gap in the chain, not merely as a
-- smaller number.
SELECT 'last rank in the chain recorded outgoing accesses' AS problem,
       run_dev, count(*) AS accesses
FROM gpu_access
WHERE CAST(run_dev AS INTEGER) = (SELECT ranks FROM params) - 1
GROUP BY run_dev;
