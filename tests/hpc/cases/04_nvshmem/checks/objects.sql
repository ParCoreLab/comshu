-- Memory objects recorded per rank.
--
-- 38 as of the core object registry (was 34). The four new rows per rank are
-- not noise and were verified against a pre-change capture of the same workload
-- (corpus nvshmem-44400747), which differs from this one in exactly these
-- sizes:
--
--   2 x 262144  — the application's own dst and src. putget calls
--                 nvshmem_malloc(65536 * 4) twice, and until the registry moved
--                 into the core NEITHER appeared here. This file's comment used
--                 to read "symmetric-heap allocations tracked per rank
--                 (nvshmem_malloc dst + src)" while the golden contained no dst
--                 and no src: the comment described an intent the data had
--                 never satisfied, and counting rows made that impossible to
--                 see. That gap is the reason the registry was moved.
--   1 x 1073741824, 1 x 536870912
--               — NVSHMEM's symmetric heap mappings, which are real regions the
--                 runtime creates. They nest around the buffers above, which is
--                 why ObjectRegistry::find takes the INNERMOST containing object
--                 rather than the first match; mem_owner_unambiguous and
--                 owner_in_range both pass on this capture, so the nesting does
--                 not cost attribution.
--
-- This stays a count rather than becoming a listing on purpose. The sizes are
-- NVSHMEM's business and change with its version; what a regression net should
-- notice is a change in how many objects a fixed workload produces.
SELECT rank, count(*) AS objects
FROM mem_object
GROUP BY rank
ORDER BY rank;
