-- requires: mem_object params.size_malloc params.loop_iters
-- Every allocation route produced exactly its objects, with the origin that
-- names the call. Built from params and LEFT JOINed onto the capture, so a route
-- that recorded NOTHING is an offending row — the usual empty-means-pass shape
-- would go green on a capture where the doors never fired.
--
-- "Exactly" is the half that catches a nested door. If cudart's call reached
-- the driver through two of the engine's wrappers (a cell wrapper reached from
-- inside the link definition, say), the depth guard must make the inner one
-- transparent; without it one cudaMalloc is two rows.
WITH expected(route, origin, size, n) AS (
  SELECT 'cudaMalloc',              'CuMemAlloc',        size_malloc,    1          FROM params UNION ALL
  SELECT 'cudaMallocManaged',       'CuMemAllocManaged', size_managed,   1          FROM params UNION ALL
  SELECT 'cudaMallocAsync',         'CuMemAllocAsync',   size_async,     1          FROM params UNION ALL
  SELECT 'cudaMallocHost',          'CuMemHost',         size_host,      1          FROM params UNION ALL
  SELECT 'cudaHostAlloc',           'CuMemHost',         size_hostalloc, 1          FROM params UNION ALL
  SELECT 'cuMemAlloc (-lcuda)',     'CuMemAlloc',        size_driver,    1          FROM params UNION ALL
  SELECT 'dlsym cuMemAlloc_v2',     'CuMemAlloc',        size_dlsym,     1          FROM params UNION ALL
  SELECT 'allocate/free loop',      'CuMemAlloc',        loop_size,      loop_iters FROM params
),
got AS (
  SELECT e.route, e.origin, e.size, e.n,
         (SELECT count(*) FROM mem_object m
          WHERE CAST(m.size AS BIGINT) = e.size AND m.origin = e.origin) AS objects
  FROM expected e
)
SELECT 'route did not produce exactly its objects' AS problem, route, origin, size,
       n AS expected, objects
FROM got
WHERE objects != n;
