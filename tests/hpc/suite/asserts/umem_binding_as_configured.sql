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
-- SCOPED TO NIC BUFFERS THAT LIVE ON A GPU, and that is a sharpening rather
-- than a relaxation.
--
-- The clause used to demand the configured binding of EVERY umem. That holds
-- for GPU-submitted IBGDA, where every NIC buffer is GPU-resident. It is false
-- by construction for CPU-assisted IBGDA, where the transport registers two
-- populations and both are correct: the GPU-resident doorbell records and
-- producer indices by dmabuf, and its own host-side control pages by address.
-- Measured on MN5 (job 45476000): 30 umems of 8 B on devices 0 and 1 by_dmabuf,
-- 422 of 4096 B with no device by_addr. Demanding one binding of all 452 failed
-- a capture that was right.
--
-- What the assertion is FOR survives intact, because the original mistake it
-- was built from — reading a by_dmabuf capture with by_addr expectations, and
-- concluding "no NIC buffer is in GPU memory", i.e. that IBGDA never engaged —
-- is about GPU buffers specifically. Those are still checked exactly as before.
-- 65535 is kUnknownDevice (core/vocab.hpp).
SELECT 'binding differs from the configured transport' AS problem,
       rank, umem_id, binding, addr
FROM mlx5_umem
WHERE device != 65535
  AND binding != (CASE WHEN (SELECT umem_binding FROM params) = 1
                       THEN 'by_dmabuf' ELSE 'by_addr' END)

UNION ALL

-- The other half of that split, and it closes the hole scoping would otherwise
-- open. A buffer the NIC reaches by dmabuf was exported FROM a device, so one
-- carrying no device means the descriptor was never resolved — which is
-- precisely how a GPU-resident buffer would masquerade as a host one and slip
-- past the clause above.
SELECT 'by_dmabuf umem with no owning device (descriptor never resolved)',
       rank, umem_id, binding, addr
FROM mlx5_umem
WHERE binding = 'by_dmabuf' AND device = 65535

UNION ALL

-- And the positive check, which no clause here ever made: SOME NIC buffer must
-- be GPU-resident. Every clause in this file is a "nothing is wrong" statement,
-- and a capture with zero umems satisfies all of them — so the exact conclusion
-- this assertion exists to prevent, "IBGDA did not engage", was the one thing it
-- could not catch. It is a statement about the run in both arms: GPU-submitted
-- and CPU-assisted IBGDA alike build their work queues in GPU memory, and only
-- who rings the doorbell differs.
-- THE UMEM POPULATION IS NOT ALWAYS WHERE THE GPU BUFFER LIVES, and asserting
-- that it is cost a false red on case 11 (du04, 2026-09-11). A fully CPU-driven
-- DEVX transport (NVSHMEM_REMOTE_TRANSPORT=ibdevx) registers its work queues
-- and doorbell records as umems — 64 KiB rings and 8 B doorbells, host-resident
-- BY DESIGN, because the CPU is what writes them — and hands the NIC its GPU
-- memory through ibv_reg_mr instead. The capture showed exactly that: 10 umems
-- per rank all at device=65535, and one 512 MiB ib_mem_region at 0x620000000
-- that mem_owner resolved to device 0 on rank 0 and device 1 on rank 1. The
-- GPU *was* handed to the NIC; this clause was looking in the wrong table and
-- said the opposite.
--
-- That third population is why the escape is a declared parameter rather than a
-- blanket relaxation. For 09/10/12 the claim is specifically that the WORK
-- QUEUES are in GPU memory — that is what GPU-submitted and CPU-assisted IBGDA
-- both do and what "IBGDA engaged" means — so allowing a GPU-resident MR to
-- satisfy them would let a capture whose queues are host-resident pass on the
-- strength of its data heap. Those cases leave umem_control_plane_only unset
-- and keep the strict umem requirement; only a case that declares its umems are
-- control-plane may point at the MR population instead.
--
-- Reading this clause's failure as "PeerMappingOverride must be missing" is the
-- trap it laid: MN5 lacks that flag and failed here, du04 has it and failed
-- identically, and neither failure was about the driver at all.
SELECT 'no NIC buffer is GPU-resident — the transport did not hand the GPU to '
       'the NIC at all', rank, NULL, NULL, NULL
FROM (SELECT DISTINCT rank FROM mlx5_umem)
WHERE rank NOT IN (SELECT rank FROM mlx5_umem WHERE device != 65535)
  AND NOT (COALESCE((SELECT umem_control_plane_only FROM params), 0) = 1
           AND rank IN (SELECT rank FROM mem_owner
                        WHERE source = 'ib_mem_region' AND device != 65535))

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
