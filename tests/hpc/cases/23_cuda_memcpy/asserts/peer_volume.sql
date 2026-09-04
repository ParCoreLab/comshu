-- The communication. Every byte that crossed a device boundary, counted the way
-- the derived layer counts it: BOTH ordinals known and DIFFERENT.
--
-- Not `kind = 5`. `kind` is what the API was asked for and the ordinal pair is
-- what the hardware did, and they can disagree in both directions — see
-- _memcpy_edges in pcomm-read/link_sql.hpp. Restricted to msg_bytes so the
-- phase-5 probe, whose classification is the open question this case exists to
-- measure, cannot move this number either way.
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
      AND CAST(dst_device AS INTEGER) != 65535
      AND CAST(src_device AS INTEGER) != CAST(dst_device AS INTEGER)
      AND CAST(bytes AS INTEGER) = (SELECT msg_bytes FROM params)
)
WHERE copies != (SELECT iters * gpus * (gpus - 1) FROM params)
   OR total  != (SELECT CAST(iters AS BIGINT) * gpus * (gpus - 1) * msg_bytes FROM params);
