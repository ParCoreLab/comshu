-- requires: gpu_access ib_post_send
-- The other half of cross_transport_shape, and the part that makes this one
-- capture rather than two: the ranks the address tracer went blind on must be
-- exactly the ranks the verbs tracer saw carrying bulk traffic. Same rank
-- space, same execution — a correlation replay cannot express, because its
-- passes are different runs.
--
-- Stated as a separation rather than a threshold: the QUIETEST node-crossing
-- rank must still post more sends than the BUSIEST non-crossing rank. Every
-- rank posts some IB traffic (NVSHMEM's bootstrap runs everywhere, ~82-91
-- sends), so any absolute cutoff would be a number read off a run. The gap is
-- three orders of magnitude and is a property of the ring, not of the fabric.
--
-- Names no rank: "crosser" is defined as "recorded no NVLink accesses", which
-- is itself derived.
SELECT * FROM (
  SELECT (SELECT min(n) FROM (SELECT count(*) AS n FROM ib_post_send
          WHERE rank NOT IN (SELECT rank FROM gpu_access) GROUP BY rank))
         AS quietest_crosser,
         (SELECT max(n) FROM (SELECT count(*) AS n FROM ib_post_send
          WHERE rank IN (SELECT rank FROM gpu_access) GROUP BY rank))
         AS busiest_non_crosser
)
WHERE quietest_crosser IS NULL
   OR busiest_non_crosser IS NULL
   OR quietest_crosser <= busiest_non_crosser;
