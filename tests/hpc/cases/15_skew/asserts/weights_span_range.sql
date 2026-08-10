-- requires: gpu_access
-- The heaviest edge must carry at least 8x the lightest.
--
-- Also a guard on the fixture. The formula's weights span 32x by construction
-- (slot >> 0 .. slot >> 5); 8x is the floor below which this capture stops
-- being a useful stress test of the matrix's log colour scale and of the
-- clustering metric, both of which are trivially satisfied by a uniform matrix.
-- Deliberately loose: which exponents appear depends on the PE count, and this
-- assertion is about the spread being real, not about its exact value.--
-- Written as a subquery + WHERE rather than a bare HAVING. sqlite 3.34 (MN5)
-- rejects HAVING without GROUP BY -- "a GROUP BY clause is required before
-- HAVING" -- while 3.51 (this laptop) accepts it, so the assertion passed every
-- local test and errored on the cluster. Any aggregate-over-the-whole-table
-- check in this suite has to be written this way.
SELECT 'edge weights too uniform to exercise a log scale' AS problem,
       lightest, heaviest
FROM (SELECT min(bytes) AS lightest, max(bytes) AS heaviest
        FROM (SELECT sum(CAST(size AS INTEGER)) AS bytes
                FROM gpu_access GROUP BY run_dev, mem_dev))
WHERE heaviest < 8 * lightest;
