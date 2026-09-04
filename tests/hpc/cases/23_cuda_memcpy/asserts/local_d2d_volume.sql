-- The same-device copy: device 0 buffer A to device 0 buffer B, once per
-- iteration. It is in the workload because it is the row that must NOT become
-- an edge, and a case performing only edge-producing copies could not tell a
-- correct filter from a missing one.
--
-- Selected by "one device, named twice" rather than by kind, and restricted to
-- msg_bytes so the phase-5 probe cannot land here whichever way CUPTI
-- classifies it (uva_bytes is msg_bytes/2 exactly so this exclusion is possible).
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
    WHERE CAST(src_device AS INTEGER) != 65535
      AND CAST(src_device AS INTEGER) = CAST(dst_device AS INTEGER)
      AND CAST(bytes AS INTEGER) = (SELECT msg_bytes FROM params)
)
WHERE copies != (SELECT iters FROM params)
   OR total  != (SELECT CAST(iters AS BIGINT) * msg_bytes FROM params);
