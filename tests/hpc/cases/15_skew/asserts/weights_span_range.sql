-- requires: gpu_access
-- The heaviest edge must carry at least 8x the lightest.
--
-- Also a guard on the fixture. The formula's weights span 32x by construction
-- (slot >> 0 .. slot >> 5); 8x is the floor below which this capture stops
-- being a useful stress test of the matrix's log colour scale and of the
-- clustering metric, both of which are trivially satisfied by a uniform matrix.
-- Deliberately loose: which exponents appear depends on the PE count, and this
-- assertion is about the spread being real, not about its exact value.
SELECT 'edge weights too uniform to exercise a log scale' AS problem,
       min(bytes) AS lightest, max(bytes) AS heaviest
FROM (SELECT sum(CAST(size AS INTEGER)) AS bytes
        FROM gpu_access GROUP BY run_dev, mem_dev)
HAVING max(bytes) < 8 * min(bytes);
