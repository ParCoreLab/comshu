-- requires: mlx5_devx_qp params.ranks
-- THE GATE, and the mirror of case 09's ibgda_engaged.
--
-- Case 09 asserts that every rank has at least one GPU-driven queue pair. This
-- case asserts the opposite half: DEVX queue pairs exist on every rank, and
-- NONE of them is device-driven, because ibdevx rings the doorbell from the
-- CPU.
--
-- Stating it as an assertion rather than a comment is the point. "ibdevx is
-- CPU-driven" is otherwise a claim in prose that no capture ever checks, and
-- the whole reason cases 10 and 11 are separate is so that transport and umem
-- binding cannot be silently conflated again. If NVSHMEM quietly promotes this
-- run to IBGDA — or falls back the other way — this assertion is what notices,
-- and case 11 would then be measuring case 10 while claiming otherwise.
--
-- Note the asymmetry with ibgda_engaged: that one guards against capturing
-- nothing, this one guards against capturing the wrong experiment. Both fail on
-- an empty table, via the rank-count clause below.
--
-- Offending rows: any rank whose queue pairs include a device-driven one, plus
-- a synthetic row if a rank produced no queue pairs at all.
SELECT rank, n_qp, n_gpu_driven FROM (
  SELECT rank,
         count(*)                                        AS n_qp,
         sum(CASE WHEN gpu_driven = 1 THEN 1 ELSE 0 END) AS n_gpu_driven
  FROM mlx5_devx_qp GROUP BY rank
)
WHERE n_gpu_driven > 0

UNION ALL

-- A rank missing from the table entirely cannot appear above, so it is counted
-- here. Without this the assertion passes on a capture where a rank recorded
-- nothing at all — which for a CPU-driven transport is the easy failure to miss,
-- since "no GPU-driven queue pairs" is also what an empty table looks like.
SELECT -1, (SELECT count(DISTINCT rank) FROM mlx5_devx_qp), -1
WHERE (SELECT count(DISTINCT rank) FROM mlx5_devx_qp) != (SELECT ranks FROM params);
