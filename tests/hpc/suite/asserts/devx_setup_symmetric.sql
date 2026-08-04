-- requires: mlx5_umem mlx5_devx_qp mlx5_devx_qp_connect params.ranks
-- SUITE-WIDE, but note what it assumes: that every rank runs the same code.
-- That holds for every DEVX case today (09, 10 and 11 all run the same putget
-- binary under a different transport or umem binding). A future case with
-- deliberately asymmetric ranks would have to override this file rather than
-- inherit it — the assertion would be wrong there, not the capture.
--
-- Every rank runs the same NVSHMEM bootstrap, so every rank must register the
-- same number of buffers, create the same number of DEVX queue pairs, and drive
-- the same number of connections. Asymmetry means a rank's DEVX traffic stopped
-- being captured — the failure this case exists to catch, and the one a total
-- structurally cannot show, because a count that is too low still looks like a
-- count.
--
-- Deliberately NOT the absolute numbers, for the reason case 06 had to learn on
-- hardware: at the *same commit*, two different racks produced 115 and 114
-- memory regions per rank while the queue-pair counts were identical. An
-- allocation-dependent number in a golden tests the allocator, not the tool.
-- Symmetry survives that; the absolute count does not.
SELECT * FROM (
  SELECT 'mlx5_umem' AS entity, min(n) AS lo, max(n) AS hi, count(*) AS ranks_seen
  FROM (SELECT rank, count(*) AS n FROM mlx5_umem GROUP BY rank)
  UNION ALL
  SELECT 'mlx5_devx_qp', min(n), max(n), count(*)
  FROM (SELECT rank, count(*) AS n FROM mlx5_devx_qp GROUP BY rank)
  UNION ALL
  SELECT 'mlx5_devx_qp_connect', min(n), max(n), count(*)
  FROM (SELECT rank, count(*) AS n FROM mlx5_devx_qp_connect GROUP BY rank)
)
WHERE lo != hi                                  -- a rank captured fewer than its peers
   OR lo = 0                                    -- or nothing at all
   OR ranks_seen != (SELECT ranks FROM params);  -- or a rank missing entirely
