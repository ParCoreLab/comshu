-- requires: mlx5_devx_qp mlx5_devx_qp_connect
-- Every connection must name a queue pair that some rank actually created.
--
-- This is the loopback-safe form of case 02's topology check, and the
-- difference matters. Case 02 asserts *no self-edges*, because on 8 ranks
-- across 2 nodes a rank connecting to itself is impossible and was the symptom
-- of the qp_num collision (758e975). Here both PEs sit behind ONE ConnectX-5,
-- so src_lid == dest_lid on every connection and a naive self-edge assertion
-- would fail on a perfectly correct capture. Node count is a property of the
-- machine, not of the tool, so the assertion has to be written for the machine
-- it runs on.
--
-- Closure is what survives: a dest_qp_num that resolves to nothing means either
-- the RTR decode read the wrong field, or a rank's queue pairs went uncaptured.
-- Both are real bugs, and neither depends on how many nodes there are.
SELECT 'dangling dest_qp_num' AS problem, c.rank, c.qp_num, c.dest_qp_num, c.dest_lid
FROM mlx5_devx_qp_connect c
WHERE c.dest_qp_num != 0
  AND NOT EXISTS (SELECT 1 FROM mlx5_devx_qp q WHERE q.qp_num = c.dest_qp_num)

UNION ALL

-- A connection whose own qp_num is not one we recorded creating. The RTR
-- command names the queue pair it modifies, so this cannot happen unless the
-- CREATE_QP for it was missed — the same gap ibgda_engaged.sql guards from the
-- other side.
SELECT 'connect on an uncreated qp_num', c.rank, c.qp_num, c.dest_qp_num, c.dest_lid
FROM mlx5_devx_qp_connect c
WHERE NOT EXISTS (SELECT 1 FROM mlx5_devx_qp q WHERE q.qp_num = c.qp_num);
