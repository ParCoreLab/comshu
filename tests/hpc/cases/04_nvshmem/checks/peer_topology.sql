-- Which GPU each GPU writes to over NVLink — the exact ring, in case 03's
-- shape. The put has each PE store to one neighbour, so PE r's stores land on
-- device (r+1)%4 and nowhere else.
--
-- This was `count(DISTINCT mem_dev) AS peers` until 2026-08-03, with a note
-- that "the mem_device ids are unreliable and run-varying" and a golden
-- asserting peers = 2 for a one-neighbour ring. Both the weakening and the
-- wrong number were the heap-attribution bug showing through a green test: the
-- second "peer" was this rank's own local src loads, given someone else's
-- device id by the positional scheme. Ownership now comes from nvshmem_ptr, so
-- the ids are answerable and this asserts them.
SELECT run_dev, mem_dev, count(*) AS n
FROM gpu_access
WHERE run_dev != mem_dev
GROUP BY run_dev, mem_dev
ORDER BY run_dev, mem_dev;
