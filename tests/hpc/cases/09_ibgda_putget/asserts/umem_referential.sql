-- requires: mlx5_umem mlx5_devx_qp
-- Referential integrity between the two halves of the DEVX control plane.
--
-- A queue pair names its send queue by wq_umem_id, and the provider resolves
-- that id to an address through a registry it builds from the umem hooks. If a
-- queue pair references an id no umem row carries, the registration was missed
-- — which is exactly what happens when only the legacy mlx5dv_devx_umem_reg is
-- hooked and the stack calls mlx5dv_devx_umem_reg_ex: measured reg=0, reg_ex=10
-- on this hardware, with six queue pairs created and zero umems recorded.
--
-- The failure is silent in every other check: symmetry still holds (all ranks
-- miss them equally), the queue pairs are still there, and only sq_base comes
-- back 0. So this assertion is the one that names the cause rather than the
-- symptom.
SELECT 'devx_qp references an unregistered umem' AS problem,
       q.rank, q.qp_num, q.wq_umem_id, q.sq_base
FROM mlx5_devx_qp q
WHERE NOT EXISTS (
  SELECT 1 FROM mlx5_umem u WHERE u.rank = q.rank AND u.umem_id = q.wq_umem_id
)

UNION ALL

-- A resolved umem must have produced a send-queue address. sq_base = 0 with a
-- known umem means the registry had the id but not the buffer — a different bug
-- from the one above, and one that would otherwise look identical downstream.
SELECT 'devx_qp with a known umem but no send-queue address',
       q.rank, q.qp_num, q.wq_umem_id, q.sq_base
FROM mlx5_devx_qp q
WHERE q.sq_base = '0x0'
  AND EXISTS (SELECT 1 FROM mlx5_umem u WHERE u.rank = q.rank AND u.umem_id = q.wq_umem_id);
