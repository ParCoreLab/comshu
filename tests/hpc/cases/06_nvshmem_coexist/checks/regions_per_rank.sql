-- Registrations are symmetric across the four ranks.
--
-- The totals check above was wrong for weeks in a way a total cannot expose: a
-- count that is too low still looks like a count. Per-rank evenness is the
-- structure of the workload — every rank runs the same NVSHMEM setup — so a
-- rank whose registrations stop being captured shows up here as an asymmetry
-- rather than as a slightly smaller number nobody questions.
SELECT rank, count(*) AS regions
FROM ib_mem_region
GROUP BY rank
ORDER BY rank;
