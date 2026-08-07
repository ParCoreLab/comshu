-- requires: mlx5_doorbell params.ranks
-- THE GATE for the data plane. Stages 1-2's doorbell hijack redirects the UAR
-- page and decodes the 8-byte store the GPU writes to ring the NIC. If this
-- table is empty the hijack captured nothing — either it did not engage (the
-- export-table patch missed), or the workload rang no doorbells. Every other
-- assertion in this case is vacuous without it, exactly as ibgda_engaged.sql
-- gates case 09's control plane.
--
-- Offending rows: a rank that recorded no doorbells at all. A rank present in
-- the table but with zero rows cannot appear in the GROUP BY, so a rank missing
-- entirely is counted by the second query — without it the assertion passes on
-- a capture where only one rank's hijack fired.
SELECT rank, count(*) AS n_doorbells
FROM mlx5_doorbell GROUP BY rank HAVING n_doorbells = 0

UNION ALL

SELECT -1 AS rank, (SELECT count(DISTINCT rank) FROM mlx5_doorbell) AS n_doorbells
WHERE (SELECT count(DISTINCT rank) FROM mlx5_doorbell) != (SELECT ranks FROM params);
