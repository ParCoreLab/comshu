-- requires: gpu_access
-- At least one ordered pair must be present in one direction only.
--
-- A guard on the FIXTURE rather than on the tool: the whole point of this case
-- is a matrix that is not symmetric, and the formula happens to produce one at
-- four PEs. If a future PE count made every edge bidirectional the case would
-- still pass every other assertion while quietly testing nothing — the same
-- silent-degradation failure that a ring-only corpus already cost us once.
--
-- Fires when NO one-way pair exists, which is why the comparison is on a count
-- of zero rather than on a listing of offenders.
SELECT 'no one-way edges — this capture cannot show asymmetry' AS problem,
       count(*) AS one_way_edges
FROM (SELECT DISTINCT CAST(run_dev AS INTEGER) AS f, CAST(mem_dev AS INTEGER) AS t
        FROM gpu_access) a
WHERE NOT EXISTS (
    SELECT 1 FROM (SELECT DISTINCT CAST(run_dev AS INTEGER) AS f,
                          CAST(mem_dev AS INTEGER) AS t FROM gpu_access) b
     WHERE b.f = a.t AND b.t = a.f)
HAVING count(*) = 0;
