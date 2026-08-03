-- requires: gpu_access params.gpus
-- EXPECTED TO FAIL — see meta.json's expect_fail_asserts. This is the
-- acceptance test for PLAN-20260803-1448-nvshmem-heap-attribution, not a
-- description of what we currently emit.
--
-- putget is a ring: each PE puts to exactly one neighbour, so each device
-- writes into exactly one other device's memory. Fan-out is 1.
--
-- The golden beside this file asserts 2, and passes, and has passed since it
-- was committed. Its own comment says "the ring put has each PE store to one
-- neighbor" — a one-neighbour ring cannot have fan-out 2. The golden had
-- captured the attribution bug and turned it into the expected result, which no
-- golden can ever detect, because the golden IS the defect. Measured: run_dev 0
-- attributed to mem_dev 65 and 66 (job 44113361) and to 238 and 239 (44111484),
-- 196608 accesses each — one real edge, split across two adjacent phantom
-- devices as the fragmented NVSHMEM 3.7 heap slides the positional lookup by
-- one region.
--
-- Do not make this pass by asserting 2. It goes green when attribution is
-- fixed, with no edit to this file — that is the whole point of writing it now.
SELECT run_dev, count(DISTINCT mem_dev) AS peers
FROM gpu_access
WHERE run_dev != mem_dev
GROUP BY run_dev
HAVING peers != 1
ORDER BY CAST(run_dev AS INTEGER);
