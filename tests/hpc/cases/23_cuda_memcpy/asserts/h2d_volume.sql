-- Host-to-device staging: one copy per GPU per iteration, msg_bytes each.
--
-- kind 1 is MemcpyKind::HostToDevice. The export carries the NUMBER and not the
-- name, in both the CSV and the Parquet, so that one assertion runs under
-- sqlite and under DuckDB — see Table<CudaMemcpy> in pcomm-read/main.cpp for
-- why gpu_access.op is the counter-example rather than the precedent. The names
-- are in the `vocabulary` derived table.
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
    WHERE CAST(kind AS INTEGER) = 1
)
WHERE copies != (SELECT iters * gpus FROM params)
   OR total  != (SELECT CAST(iters AS BIGINT) * gpus * msg_bytes FROM params);
