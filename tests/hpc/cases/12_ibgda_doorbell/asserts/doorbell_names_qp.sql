-- requires: mlx5_doorbell mlx5_devx_qp
-- Data-plane closure: every doorbell must name a queue pair some rank actually
-- created. This is the device-plane twin of topology_closed.sql — the same
-- detector pointed at the doorbell stream instead of the connection stream.
--
-- A doorbell whose qp_num resolves to no Mlx5DevxQp means the control-plane
-- CREATE_QP for it went uncaptured, or the doorbell decode read qpn from the
-- wrong bits of the control segment. Both are real bugs and neither depends on
-- any specific qp_num, so this names none.
SELECT DISTINCT 'doorbell on uncreated qp_num' AS problem, d.qp_num
FROM mlx5_doorbell d
WHERE NOT EXISTS (SELECT 1 FROM mlx5_devx_qp q WHERE q.qp_num = d.qp_num);
