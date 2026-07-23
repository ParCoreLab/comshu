-- Each rank registers exactly one MR (the pingpong buffer) with the fabric.
SELECT rank, count(*) AS regions
FROM ib_mem_region
GROUP BY rank
ORDER BY rank;
