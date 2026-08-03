-- requires: gpu_access
-- put_to_next's only peer access is the store into the neighbour's symmetric
-- heap; `src[i]` reads the PE's own buffer and gpu_access is peer-only
-- (engine.hpp's RemoteFilter drops run_device == mem_device).
--
-- This case is the evidence that the claim is right, which is why the same file
-- is marked expect_fail on 04 and 06 and is not marked here. Same binary, same
-- kernel filter, correct attribution: 196608 stores per non-crossing rank and
-- zero loads, in jobs 44113361 and 44116827. The single-node cases report an
-- extra 196608 loads per rank against the identical workload — so the doubled
-- count there is attribution, not the program.
SELECT op, count(*) AS accesses, count(DISTINCT rank) AS ranks
FROM gpu_access
WHERE op <> 'store'
GROUP BY op
ORDER BY op;
