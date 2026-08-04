-- requires: mlx5_devx_qp params.ranks
-- THE GATE. Every other assertion in this case is vacuous if this one does not
-- hold, because "IBGDA ran and we captured nothing" and "IBGDA never ran" are
-- the same empty tables.
--
-- This is not a hypothetical. A probe earlier this week counted 159 DEVX
-- CREATE_QP commands on a run where IBGDA had failed to initialise outright
-- ("neither nv_peer_mem, or nvidia_peermem detected. Skipping transport") and
-- reported that as proof it had engaged — the DEVX commands belonged to
-- HPC-X/UCX, which drives DEVX for ordinary MPI. A queue-pair count is not
-- evidence of GPU-initiated networking.
--
-- What IS evidence is a queue pair whose send queue the GPU can write to. A
-- CPU-side DEVX queue pair cannot ring a GPU doorbell, so gpu_driven is the
-- discriminator, and every rank must have at least one.
--
-- Offending rows: any rank that ran without a device-driven queue pair, plus a
-- synthetic row if a rank produced no queue pairs at all.
SELECT rank, n_qp, n_gpu_driven FROM (
  SELECT rank,
         count(*)                          AS n_qp,
         sum(CASE WHEN gpu_driven = 1 THEN 1 ELSE 0 END) AS n_gpu_driven
  FROM mlx5_devx_qp GROUP BY rank
)
WHERE n_gpu_driven = 0

UNION ALL

-- A rank missing from the table entirely cannot appear above, so it is counted
-- here. Without this the assertion passes on a capture where half the ranks
-- recorded nothing.
SELECT -1, (SELECT count(DISTINCT rank) FROM mlx5_devx_qp), -1
WHERE (SELECT count(DISTINCT rank) FROM mlx5_devx_qp) != (SELECT ranks FROM params);
