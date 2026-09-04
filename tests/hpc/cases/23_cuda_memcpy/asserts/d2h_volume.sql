-- Device-to-host staging, the mirror of h2d_volume. Both directions are checked
-- rather than one, because the backend derives src_device and dst_device from
-- copyKind through two separate predicates (src_is_device / dst_is_device in
-- cupti_backend.cpp) and a mistake in either would leave the other correct.
-- PORTABLE SHAPE, and it is not cosmetic: `HAVING` without `GROUP BY` is
-- accepted by DuckDB and REFUSED by sqlite, and sql_check.sh uses whichever
-- engine the export allows. The aggregate therefore sits in a subquery that
-- always yields exactly one row, and the outer WHERE decides whether it is an
-- offending row -- which keeps the property this file needs: over ZERO input
-- rows the subquery still returns a row, with copies = 0, and the check fails
-- rather than passing vacuously.
-- The byte product is CAST TO BIGINT: meta.json's integers bind as INT32 in
-- DuckDB and this expression exceeds it, which errors rather than failing.
-- See copy_edge_floor.sql, where it was measured.
SELECT * FROM (
    SELECT count(*) AS copies, sum(CAST(bytes AS BIGINT)) AS total
    FROM cuda_memcpy
    WHERE CAST(kind AS INTEGER) = 2
)
WHERE copies != (SELECT iters * gpus FROM params)
   OR total  != (SELECT CAST(iters AS BIGINT) * gpus * msg_bytes FROM params);
