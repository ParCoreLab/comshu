-- requires: gpu_access
-- The hub answers with less than it receives — by construction an eighth
-- (resp = slot/8), per spoke per iteration.
--
-- The assertion is deliberately weak about the ratio and strict about the
-- direction: bytes in must exceed bytes out. A tighter equality would encode
-- the kernel's launch geometry, which is the workload's business; what must
-- hold for the TOOL to be correct is that the two directions are not confused.
-- Any aggregation that symmetrises an edge fails this, and nothing in the
-- symmetric corpus could.
SELECT 'hub does not receive strictly more than it sends' AS problem,
       (SELECT sum(CAST(size AS INTEGER)) FROM gpu_access
         WHERE CAST(mem_dev AS INTEGER) = 0) AS bytes_into_hub,
       (SELECT sum(CAST(size AS INTEGER)) FROM gpu_access
         WHERE CAST(run_dev AS INTEGER) = 0) AS bytes_out_of_hub
WHERE (SELECT sum(CAST(size AS INTEGER)) FROM gpu_access
        WHERE CAST(mem_dev AS INTEGER) = 0)
   <= (SELECT sum(CAST(size AS INTEGER)) FROM gpu_access
        WHERE CAST(run_dev AS INTEGER) = 0);
