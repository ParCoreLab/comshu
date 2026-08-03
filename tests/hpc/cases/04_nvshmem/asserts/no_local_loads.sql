-- requires: gpu_access
-- EXPECTED TO FAIL — see meta.json's expect_fail_asserts. Second face of the
-- same bug as peer_fanout.sql, blocked on
-- PLAN-20260803-1448-nvshmem-heap-attribution.
--
-- put_to_next does one peer access per element: the store into the neighbour's
-- symmetric heap. Its other memory reference, `src[i]`, reads the PE's OWN
-- symmetric buffer, and gpu_access is peer-only by construction (engine.hpp's
-- RemoteFilter drops run_device == mem_device). A `load` row here is therefore
-- not a smaller kind of traffic; it is a local access that the fragmented
-- NVSHMEM 3.7 heap made look remote.
--
-- The evidence is 07_nvshmem_multinode, which runs this exact binary and whose
-- attribution is correct: 196608 stores per non-crossing rank and ZERO loads
-- (jobs 44113361 and 44116827). This case reports 196608 loads AND 196608
-- stores — the doubled access count that went unexplained for three weeks, sat
-- in the golden the whole time, and is one query away from being obvious.
SELECT op, count(*) AS accesses, count(DISTINCT rank) AS ranks
FROM gpu_access
WHERE op <> 'store'
GROUP BY op
ORDER BY op;
