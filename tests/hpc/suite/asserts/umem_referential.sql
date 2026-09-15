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
--
-- AND IT IS CONDITIONED ON THE ID BEING UNAMBIGUOUS, which the first version of
-- this clause was not, and which cost a red board on a correct capture (case 25,
-- job 45584953). **umem_id is recycled.** The NIC hands ids back out as buffers
-- are registered and deregistered, so one (rank, umem_id) can name several
-- different buffers over a run, with different bindings. And it is not unique
-- even at one instant: each HCA has its own id space, so a rank that opens six
-- of them (UCX on MN5, job 45886136) has four queue pairs naming wq_umem_id 20.
-- The provider keys its registry on (context, id) for exactly this reason; the
-- rows carry no context, so this join cannot. Rank 1 of job 45584953's capture
-- has FIVE rows for umem_id 51:
--
--   addr=0x4383000 size=131072 device=65535 by_addr
--   addr=0x455d000 size=131072 device=65535 by_addr
--   addr=0x490d000 size=131072 device=65535 by_addr
--   addr=0x0       size=8      device=1     by_dmabuf
--   addr=0x0       size=65536  device=1     by_dmabuf
--
-- The queue pair that references id 51 there is GPU-driven on device 1 with
-- log_sq_size=10 — 1024 WQEBBs, 65536 bytes — so it is unmistakably the last
-- row, a dmabuf buffer whose sq_base of 0x0 is the legitimate offset this
-- clause already knows to allow. `EXISTS (... binding = 'by_addr')` matched one
-- of the recycled host registrations instead and reported a defect that was not
-- there.
--
-- So: fire only when EVERY row for that id is by_addr. If the id also names a
-- dmabuf buffer, the join cannot tell which one the queue pair means, and an
-- assertion that cannot tell must not claim. That is a real loss of power —
-- a genuine by_addr failure on a recycled id now goes unreported — and it is
-- the right trade, because the alternative is a check that goes red on captures
-- that are correct, which is how a board stops being read at all.
--
-- Doing better needs the umem the queue pair actually referenced, which means
-- resolving the id AT THE TIME OF THE CREATE_QP rather than over the whole
-- capture. The provider knows that — it holds a live registry — and the record
-- does not carry it. A `wq_umem` handle on Mlx5DevxQp pointing at the specific
-- Mlx5Umem row would make this exact, and is worth doing the next time that
-- record changes.
SELECT 'devx_qp with a known by_addr umem but no send-queue address',
       q.rank, q.qp_num, q.wq_umem_id, q.sq_base
FROM mlx5_devx_qp q
WHERE q.sq_base = '0x0'
  AND EXISTS (SELECT 1 FROM mlx5_umem u
               WHERE u.rank = q.rank AND u.umem_id = q.wq_umem_id
                 AND u.binding = 'by_addr')
  AND NOT EXISTS (SELECT 1 FROM mlx5_umem u
                   WHERE u.rank = q.rank AND u.umem_id = q.wq_umem_id
                     AND u.binding != 'by_addr');
