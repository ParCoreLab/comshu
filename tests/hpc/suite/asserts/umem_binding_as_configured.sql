-- requires: mlx5_umem mlx5_devx_qp params.umem_binding
-- The capture must agree with the transport that was configured.
--
-- This assertion exists because of a specific, expensive mistake. IBGDA hands
-- buffers to the NIC one of two ways, and both are correct:
--
--   by_addr    peermem path (NVSHMEM_IB_DISABLE_DMABUF=1). The umem carries a
--              real pointer, so the owning GPU can be resolved by asking CUDA
--              about it.
--   by_dmabuf  the buffer is named by file descriptor. The address field is an
--              OFFSET, observed as 0, and there is no pointer to classify.
--
-- Reading a by_dmabuf capture with by_addr expectations produces "no NIC buffer
-- is in GPU memory" — which reads as *IBGDA did not engage* and is completely
-- wrong. That is precisely how SigmaTrace was misjudged as version-stale here:
-- it was run in the configuration it does not support, and the resulting crash
-- was read as confirmation of a prediction. Pinning the configured binding as
-- an oracle input makes that class of error a red assertion instead of a
-- conclusion.
--
-- The address check is the load-bearing half: under by_dmabuf a nonzero address
-- would mean the provider is reporting an offset as if it were a pointer, which
-- is the silent-wrong-value failure this suite keeps finding.
SELECT 'binding differs from the configured transport' AS problem,
       rank, umem_id, binding, addr
FROM mlx5_umem
WHERE binding != (CASE WHEN (SELECT umem_binding FROM params) = 1
                       THEN 'by_dmabuf' ELSE 'by_addr' END)

UNION ALL

SELECT 'by_dmabuf umem with a nonzero address (offset reported as a pointer)',
       rank, umem_id, binding, addr
FROM mlx5_umem
WHERE binding = 'by_dmabuf' AND addr != '0x0'

UNION ALL

SELECT 'by_dmabuf umem without a descriptor', rank, umem_id, binding, dmabuf_fd
FROM mlx5_umem
WHERE binding = 'by_dmabuf' AND dmabuf_fd < 0

UNION ALL

SELECT 'by_addr umem carrying a descriptor', rank, umem_id, binding, dmabuf_fd
FROM mlx5_umem
WHERE binding = 'by_addr' AND dmabuf_fd >= 0;
