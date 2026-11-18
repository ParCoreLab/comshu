-- requires: mem_object entity_close params.size_malloc params.loop_iters
-- Each route's object is closed exactly as often as the program freed it —
-- per route, so a close credited to the wrong object cannot balance out.
--
-- Three of these rows are the behaviours the doors could get wrong while every
-- allocation is still recorded:
--   cudaMalloc   freed TWICE by the workload; the driver rejects the second,
--                and the engine must close once — it closes only a LIVE object
--   managed      never freed; zero closes, and the suite does not call that a
--                defect (a buffer alive at exit is legal)
--   loop         loop_iters frees at one size, all at one reused base: as many
--                closes as opens, which is the registry reviving its entry
--   cuMemMap     two mappings, ONE cuMemUnmap over both: two closes
--
-- Credited by id. The workload keeps every route's buffer live until the loop
-- has run (see allocdoors.cu), so no two routes share an address — and therefore
-- an interned id — and a close cannot leak from one route's count to another's.
WITH expected(route, origin, size, closes) AS (
  SELECT 'cudaMalloc (freed twice)', 'CuMemAlloc',        size_malloc,    1          FROM params UNION ALL
  SELECT 'cudaMallocManaged (kept)', 'CuMemAllocManaged', size_managed,   0          FROM params UNION ALL
  SELECT 'cudaMallocAsync',          'CuMemAllocAsync',   size_async,     1          FROM params UNION ALL
  SELECT 'cudaMallocHost',           'CuMemHost',         size_host,      1          FROM params UNION ALL
  SELECT 'cudaHostAlloc',            'CuMemHost',         size_hostalloc, 1          FROM params UNION ALL
  SELECT 'cuMemAlloc (-lcuda)',      'CuMemAlloc',        size_driver,    1          FROM params UNION ALL
  SELECT 'dlsym cuMemAlloc_v2',      'CuMemAlloc',        size_dlsym,     1          FROM params UNION ALL
  SELECT 'allocate/free loop',       'CuMemAlloc',        loop_size,      loop_iters FROM params
),
ids AS (
  SELECT DISTINCT e.route, m.rank, m.id
  FROM expected e JOIN mem_object m
    ON CAST(m.size AS BIGINT) = e.size AND m.origin = e.origin
),
got AS (
  SELECT e.route, e.closes,
         (SELECT count(*) FROM entity_close c JOIN ids i
            ON CAST(c.rank AS INTEGER) = CAST(i.rank AS INTEGER) AND c.subject = i.id
          WHERE i.route = e.route AND CAST(c.opener AS INTEGER) = 520) AS got
  FROM expected e
)
SELECT 'route closed a different number of times than it was freed' AS problem,
       route, closes AS expected, got
FROM got WHERE got != closes
UNION ALL
SELECT 'one cuMemUnmap did not close both mappings', 'cuMemMap', 2,
       (SELECT count(*) FROM entity_close c JOIN mem_object m
          ON CAST(c.rank AS INTEGER) = CAST(m.rank AS INTEGER) AND c.subject = m.id
        WHERE m.origin = 'CuMemMap' AND CAST(c.opener AS INTEGER) = 520)
WHERE (SELECT count(*) FROM entity_close c JOIN mem_object m
          ON CAST(c.rank AS INTEGER) = CAST(m.rank AS INTEGER) AND c.subject = m.id
        WHERE m.origin = 'CuMemMap' AND CAST(c.opener AS INTEGER) = 520) != 2;
