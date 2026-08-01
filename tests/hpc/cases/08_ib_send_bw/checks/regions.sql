-- Memory regions, and the reason this case has to assert them.
--
-- verbs.h makes ibv_reg_mr a macro over a static inline that falls through to
-- ibv_reg_mr_iova2 when the access flags carry an IBV_ACCESS_OPTIONAL_RANGE bit
-- *or* when the access argument is not a compile-time constant. perftest hits
-- both. Interposing only the legacy ibv_reg_mr symbol therefore captured
-- literal-flag callers (case 01's ibv_rc_pingpong) and missed ordinary ones:
-- before ibv_reg_mr_iova2 was hooked this query returned nothing at all, and no
-- check in the suite noticed, because case 01 asserts MRs and takes the other
-- path.
--
-- Oracle: `ib_send_bw -s 8192` registers one region per rank of 2 x 8192 B
-- (perftest double-buffers send and receive for its single QP). has_optional is
-- the bit that routes registration to iova2 — asserting it is what pins this
-- case to the path it exists to cover, rather than to memory regions in general.
SELECT rank,
       count(*) AS regions,
       sum(CAST(size AS INTEGER)) AS bytes,
       min(CASE WHEN (CAST(access AS INTEGER) & 1048576) <> 0 THEN 1 ELSE 0 END)
           AS has_optional
FROM ib_mem_region
GROUP BY rank
ORDER BY rank;
