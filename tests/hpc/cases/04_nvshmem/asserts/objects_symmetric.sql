-- requires: mem_object params.ranks
-- Every rank records the same, nonzero number of memory objects.
--
-- The count itself is not derivable — it is the library's own allocations
-- (NVSHMEM's heap carves dozens; NCCL's p2p path, its buffers) — so the golden
-- this replaced pinned a number from one run. The SHAPE is derivable: the
-- ranks run the same program, so a rank with fewer objects lost some, and a
-- rank with none lost its instrumentation.
SELECT 'asymmetric' AS problem, CAST(rank AS INTEGER) AS rank, count(*) AS objects
FROM mem_object
GROUP BY rank
HAVING count(*) != (SELECT max(n) FROM (SELECT count(*) AS n FROM mem_object GROUP BY rank))

UNION ALL

SELECT 'missing', NULL, (SELECT count(DISTINCT rank) FROM mem_object)
WHERE (SELECT count(DISTINCT rank) FROM mem_object) != (SELECT ranks FROM params);
