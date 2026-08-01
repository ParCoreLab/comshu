-- IB side, single-node: transport setup only. Registered memory regions and
-- QP state transitions still prove the ib provider's hooks fired inside a
-- process that NVBit also owns — which is exactly what used to segfault.
-- (ib_post_send / ib_poll_cq are legitimately empty at one node; see meta.json.)
--
-- mem_region was 16 here until 2026-08-01, and 16 was wrong. It was not what
-- NVSHMEM registers; it was what our hook could see. `ibv_reg_mr` is a macro
-- that only reaches the legacy exported symbol when the access flags are a
-- compile-time constant with no IBV_ACCESS_OPTIONAL_RANGE bit, and we hooked
-- only that symbol — so 96% of the registrations were invisible and the number
-- that was missing them got committed as "verified".
--
-- 460 is now measured through ibv_reg_mr_iova2, the point every path funnels
-- through: reproduced exactly across three independent runs (44056257, 44056539,
-- 44056659), with zero byte-identical rows, so nothing is double-counted. The
-- 31 repeated mr handles carry different lkeys — freed pointers reused for new
-- registrations, both visible because dereg emits no close record yet.
SELECT 'mem_region' AS entity, count(*) AS n FROM ib_mem_region
UNION ALL SELECT 'queue_pair', count(*) FROM ib_queue_pair
UNION ALL SELECT 'modify_qp',  count(*) FROM ib_modify_qp
ORDER BY entity;
