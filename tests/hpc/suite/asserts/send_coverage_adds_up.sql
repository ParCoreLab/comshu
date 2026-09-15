-- requires: mlx5_send_coverage
-- A door's coverage row adds up.
--
-- Every WQEBB the producers published past a watched queue's starting point is
-- exactly one of decoded, lapped or dropped (Mlx5SendCoverage in
-- providers/mlx5dv/mlx5_records.hpp), so
--
--     published == decoded + lapped + dropped
--
-- holds for ANY run, lossy or not: a large one may lap the ring and still add
-- up. What breaks it is a WQEBB the watcher neither decoded nor counted as lost
-- — staged and never decoded before the process exited, say — which is the
-- silent loss this record exists to make impossible. Cross-cutting, so it runs
-- wherever the watcher ran; HOW MUCH may be lost is a case's own claim, and
-- case 11 makes it (data_plane_covered.sql demands none).
--
-- An invariant that names no rank, queue pair or device: sql_check.sh's rule
-- for an assertion that does not read `params`. Only coverage columns, which
-- are the same in the CSV and the Parquet export.
SELECT rank, submitter, published, decoded, lapped, dropped
FROM mlx5_send_coverage
WHERE CAST(published AS INTEGER)
      != CAST(decoded AS INTEGER) + CAST(lapped AS INTEGER) + CAST(dropped AS INTEGER);
