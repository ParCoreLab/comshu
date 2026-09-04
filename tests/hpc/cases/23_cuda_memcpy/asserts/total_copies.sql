-- EVERY copy the workload performed, in one number derived entirely from
-- params. This is the case's load-bearing assertion and the reason none of the
-- others can pass on an empty table by accident:
--
--   2*iters*gpus   staging (H2D then D2H, once per GPU per iteration)
--   + iters        the same-device D2D, on device 0
--   + iters*gpus*(gpus-1)   the peer phase, every ordered pair
--   + iters        the phase-5 cross-device probe (gpus >= 2)
--
-- A bare count(*) against that expression, so an empty capture compares 0 and
-- FAILS — which is what a suite full of checks whose convention is "empty means
-- pass" must be paired with. See the vacuous-pass section of
-- tests/hpc/suite/README.md.
-- PORTABLE SHAPE, and it is not cosmetic: `HAVING` without `GROUP BY` is
-- accepted by DuckDB and REFUSED by sqlite, and sql_check.sh uses whichever
-- engine the export allows. The aggregate therefore sits in a subquery that
-- always yields exactly one row, and the outer WHERE decides whether it is an
-- offending row -- which keeps the property this file needs: over ZERO input
-- rows the subquery still returns a row, with copies = 0, and the check fails
-- rather than passing vacuously.
SELECT * FROM (
    SELECT count(*) AS copies FROM cuda_memcpy
)
WHERE copies != (SELECT 2 * iters * gpus
                      + iters
                      + iters * gpus * (gpus - 1)
                      + CASE WHEN gpus >= 2 THEN iters ELSE 0 END
                 FROM params);
