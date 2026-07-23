-- Symmetric-heap allocations tracked per rank (nvshmem_malloc dst + src).
SELECT rank, count(*) AS objects
FROM mem_object
GROUP BY rank
ORDER BY rank;
