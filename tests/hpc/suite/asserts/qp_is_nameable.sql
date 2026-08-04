-- requires: mlx5_devx_qp_connect
-- A connected queue pair must know its own port's LID.
--
-- This is not bookkeeping. qp_num is allocated per HCA, so two ranks on
-- different nodes routinely draw the same one — 199 collisions in a single
-- 8-rank NCCL capture — and a reader keying peers on qp_num alone attributes
-- edges to whichever rank it scanned last, including impossible self-edges.
-- Case 02 measured exactly that damage and it cost a record-type bump to fix
-- (758e975). (src_lid, qp_num) is what names a queue pair uniquely, because
-- qp_num IS unique within one HCA.
--
-- So a connection that names a remote endpoint but carries src_lid = 0 has
-- produced a row nothing downstream can join on correctly. Scoped to
-- dest_lid != 0 deliberately: a DCI's ->RTR names no remote (dynamically
-- connected initiators resolve their destination per work request), so those
-- rows legitimately carry zeros and are not evidence of anything.
SELECT 'connected qp with no source LID — (src_lid, qp_num) cannot name it' AS problem,
       rank, qp_num, dest_qp_num, dest_lid, src_lid, port_num
FROM mlx5_devx_qp_connect
WHERE dest_lid != 0 AND src_lid = 0;
