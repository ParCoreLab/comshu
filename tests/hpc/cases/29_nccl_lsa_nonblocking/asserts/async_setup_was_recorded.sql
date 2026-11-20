-- requires: params.ranks params.window_bytes
-- What NCCL finished on its own thread still reached the capture (CR-25).
--
-- Every row here is one the provider can only write AFTER the call that asked
-- for it returned: the communicator's rank (filled in by init's async half),
-- the window (its handle written back after ncclGroupEnd answered
-- ncclInProgress), and each peer's mapping of it (valid only once the
-- communicator is ready). Before the fix all three were absent on this
-- workload, and the placement asserts failed with no word on why; this one
-- says which.
--
-- Deliberately NOT `requires: nccl_comm nccl_window ...`: a required table with
-- no rows makes the check SKIP, and missing rows are the failure under test.
-- params-only requires keeps it running, and the counts below read 0.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  -- One communicator per rank, its own rank and the full size, as NCCL
  -- reported them once ready rather than the placeholders it holds before.
  SELECT 'nccl_comm rows' AS problem, r AS rank,
         (SELECT count(*) FROM nccl_comm c WHERE CAST(c.rank AS INTEGER) = r) AS n, 1 AS expected
  FROM expected
  WHERE (SELECT count(*) FROM nccl_comm c WHERE CAST(c.rank AS INTEGER) = r) != 1

  UNION ALL
  SELECT 'nccl_comm not ready when recorded', CAST(rank AS INTEGER), nccl_rank, NULL
  FROM nccl_comm
  WHERE CAST(nranks AS INTEGER) != (SELECT ranks FROM params)
     OR CAST(nccl_rank AS INTEGER) < 0
     OR CAST(nccl_rank AS INTEGER) >= (SELECT ranks FROM params)

  -- One window per rank, of the size the workload registered.
  UNION ALL
  SELECT 'nccl_window rows', r,
         (SELECT count(*) FROM nccl_window w
          WHERE CAST(w.rank AS INTEGER) = r
            AND CAST(w.size AS INTEGER) = (SELECT window_bytes FROM params)), 1
  FROM expected
  WHERE (SELECT count(*) FROM nccl_window w
         WHERE CAST(w.rank AS INTEGER) = r
           AND CAST(w.size AS INTEGER) = (SELECT window_bytes FROM params)) != 1

  -- Every other rank's copy of it, mapped in this process: one node, so all
  -- of them are in the LSA team.
  UNION ALL
  SELECT 'nccl_window_peer rows', r,
         (SELECT count(DISTINCT p.peer) FROM nccl_window_peer p WHERE CAST(p.rank AS INTEGER) = r),
         (SELECT ranks - 1 FROM params)
  FROM expected
  WHERE (SELECT count(DISTINCT p.peer) FROM nccl_window_peer p WHERE CAST(p.rank AS INTEGER) = r)
        != (SELECT ranks - 1 FROM params)
)
ORDER BY problem, rank;
