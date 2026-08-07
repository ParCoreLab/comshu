-- requires: mlx5_device_send mlx5_devx_qp mlx5_doorbell
-- Every decoded send must (a) name a queue pair that was created, and (b) belong
-- to a queue pair that rang at least one doorbell — because a Mlx5DeviceSend is
-- produced only by parsing the WQEs a doorbell announced, so a send on a qp_num
-- with no doorbell would mean the parser invented it.
--
-- Names no specific qp_num; both are structural invariants of the decode.
SELECT 'send on uncreated qp_num' AS problem, s.qp_num
FROM mlx5_device_send s
WHERE NOT EXISTS (SELECT 1 FROM mlx5_devx_qp q WHERE q.qp_num = s.qp_num)

UNION ALL

SELECT 'send on a qp that rang no doorbell' AS problem, s.qp_num
FROM mlx5_device_send s
WHERE NOT EXISTS (SELECT 1 FROM mlx5_doorbell d WHERE d.qp_num = s.qp_num);
