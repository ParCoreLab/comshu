-- Intra-node peer map: which GPU touched which *other* GPU's memory over NVLink
-- (run_dev != mem_dev). This is the signal only the p2p/gpumem provider sees;
-- it must be non-empty here and empty in the pure-IB case 02.
SELECT run_dev, mem_dev, count(*) AS n
FROM gpu_access
WHERE run_dev != mem_dev
GROUP BY run_dev, mem_dev
ORDER BY run_dev, mem_dev;
