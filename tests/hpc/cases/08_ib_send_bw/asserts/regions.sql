-- requires: ib_mem_region params.regions_per_rank
-- One registered region per endpoint. Byte count is deliberately absent: it is
-- 16384 = 2*8192 because perftest double-buffers send and receive for its
-- single QP, and "2*size" is a guess at an implementation detail rather than a
-- derivation from the command line. The golden keeps that number; an assertion
-- would only be dressing it up.
--
-- What IS derivable is the count, and the count is what went to zero: before
-- ibv_reg_mr_iova2 was hooked this table was empty for perftest, because
-- verbs.h's ibv_reg_mr macro only reaches the legacy exported symbol when the
-- access flags are a compile-time constant without an IBV_ACCESS_OPTIONAL_RANGE
-- bit. Case 01 takes the literal path and so could not notice.
SELECT rank, count(*) AS regions
FROM ib_mem_region
GROUP BY rank
HAVING regions != (SELECT regions_per_rank FROM params)
ORDER BY CAST(rank AS INTEGER);
