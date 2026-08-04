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

-- A resolved umem must have produced a send-queue address — but ONLY when the
-- buffer was registered by address. This clause originally had no such
-- condition and failed on a correct capture, which is worth keeping as a
-- comment because the reason is the whole point of the binding field.
--
-- Under by_dmabuf the umem's address is 0 by construction, so sq_base is an
-- OFFSET within the descriptor rather than a pointer. The real capture shows
-- three queue pairs carved out of one 327680-byte buffer at 0x0, 0x10000 and
-- 0x20000 — and 0x0 is the legitimate first slot, not a failed lookup. Demanding
-- a nonzero sq_base there asserts that the first queue pair cannot exist.
--
-- So this is conditioned on the binding. Under by_addr a zero base still means
-- the registry had the id but not the buffer, which is a real defect.
SELECT 'devx_qp with a known by_addr umem but no send-queue address',
       q.rank, q.qp_num, q.wq_umem_id, q.sq_base
FROM mlx5_devx_qp q
WHERE q.sq_base = '0x0'
  AND EXISTS (SELECT 1 FROM mlx5_umem u
               WHERE u.rank = q.rank AND u.umem_id = q.wq_umem_id
                 AND u.binding = 'by_addr');
