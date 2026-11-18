-- requires: mem_object params.gpus
-- One tracked allocation per GPU: the process cudaMalloc's a buffer on each
-- device before the ring runs, so a correct capture has exactly `gpus` of them
-- and no more. Derived from the GPU count, not counted off a previous run.
--
-- The upper bound is the useful half. Under-counting allocations makes device
-- attribution silently approximate — an access whose address matches no tracked
-- object has to be attributed by some other means — and that is precisely the
-- failure mode 04's fragmented symmetric heap turned into out-of-range mem_dev
-- values.
--
-- BUFFERS, NOT ROWS. This counted rows, which was the same number only while
-- gpumem was the sole emitter of a plain allocation. Since PLAN-20261118-1930
-- the engine records every cudaMalloc too, so each buffer here has two rows —
-- origin NvbitAlloc and origin CuMemAlloc, one range, one id — and the case
-- reported 8 objects for 4 buffers (MN5 job 49207744). A buffer is a
-- (base, size) extent; the second clause checks that both emitters saw each
-- one, which is the property the doubled count was actually evidence of.
SELECT rank, count(DISTINCT base || ':' || size) AS objects
FROM mem_object
GROUP BY rank
HAVING objects != (SELECT gpus FROM params)
UNION ALL
SELECT rank, count(*) AS objects
FROM (SELECT rank, base, size FROM mem_object
      GROUP BY rank, base, size
      HAVING count(DISTINCT origin) != 2
          OR sum(origin = 'NvbitAlloc') != 1 OR sum(origin = 'CuMemAlloc') != 1)
GROUP BY rank
ORDER BY 1;
