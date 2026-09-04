-- ... and there are exactly gpus*(gpus-1) of those pairs. peer_matrix checks
-- the count on every pair it FINDS, which is silent about a pair that is
-- missing entirely — the failure a transposed src/dst would produce if it
-- happened to be symmetric.
-- PORTABLE SHAPE, and it is not cosmetic: `HAVING` without `GROUP BY` is
-- accepted by DuckDB and REFUSED by sqlite, and sql_check.sh uses whichever
-- engine the export allows. The aggregate therefore sits in a subquery that
-- always yields exactly one row, and the outer WHERE decides whether it is an
-- offending row -- which keeps the property this file needs: over ZERO input
-- rows the subquery still returns a row, with copies = 0, and the check fails
-- rather than passing vacuously.
SELECT * FROM (
    SELECT count(*) AS pairs FROM (
        SELECT DISTINCT src_device, dst_device
        FROM cuda_memcpy
        WHERE CAST(src_device AS INTEGER) != 65535
          AND CAST(dst_device AS INTEGER) != 65535
          AND CAST(src_device AS INTEGER) != CAST(dst_device AS INTEGER)
          AND CAST(bytes AS INTEGER) = (SELECT msg_bytes FROM params)
    )
)
WHERE pairs != (SELECT gpus * (gpus - 1) FROM params);
