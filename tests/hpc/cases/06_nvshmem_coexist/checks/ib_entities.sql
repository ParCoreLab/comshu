-- IB side, single-node: transport setup only. Registered memory regions and
-- QP state transitions still prove the ib provider's hooks fired inside a
-- process that NVBit also owns — which is exactly what used to segfault.
-- (ib_post_send / ib_poll_cq are legitimately empty at one node; see meta.json.)
SELECT 'mem_region' AS entity, count(*) AS n FROM ib_mem_region
UNION ALL SELECT 'queue_pair', count(*) FROM ib_queue_pair
UNION ALL SELECT 'modify_qp',  count(*) FROM ib_modify_qp
ORDER BY entity;
