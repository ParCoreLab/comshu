-- Ring PGAS pattern: each PE writes into the next PE's symmetric buffer over
-- NVLink (run_dev != mem_dev). Confirms cross-GPU PGAS traffic was captured.
SELECT run_dev, mem_dev, count(*) AS n
FROM gpu_access
WHERE run_dev != mem_dev
GROUP BY run_dev, mem_dev
ORDER BY run_dev, mem_dev;
