-- Tracked device allocations per rank (the buffers NCCL registered).
SELECT rank, count(*) AS objects
FROM mem_object
GROUP BY rank
ORDER BY rank;
