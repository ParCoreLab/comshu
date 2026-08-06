-- requires: ib_mem_region mem_object params.gpudirect
-- What the NIC registers must match the transport the host can actually offer.
--
-- This assertion used to be called symmetric_heap_is_device and asserted, flatly,
-- that the largest region each rank registers is device-resident "by
-- construction of the transport". That was an assumption written down as an
-- oracle, and MN5 refused it: job 44348796 reported device=65535 on every 38 MB
-- region, and the diagnostic (PCOMM_TRACE_HOOKS=1, job 44349630) said
-- `stage=called rc=1 type=0` on all 8 ranks — libcuda resolved, the call ran,
-- and the pointer is not known to CUDA at all. The buffers are ordinary host
-- memory and 65535 was the right answer.
--
-- The mechanism is a strong inference rather than a measurement: MN5 has neither
-- nvidia_peermem nor PeerMappingOverride=1, so GPUDirect RDMA is unavailable and
-- NVSHMEM stages inter-node traffic through host bounce buffers. Both facts
-- about MN5 are measured (cases 09/10 skip on exactly those checks); the causal
-- link between them and these buffers is not.
--
-- So residency becomes an ORACLE INPUT, the same discipline as
-- umem_binding_as_configured: the case declares which transport the host can
-- offer, and the capture is checked against that declaration in BOTH directions.
-- This is not a weakening. Before, one configuration was checked and the other
-- was unrepresentable; now each is checked, and a capture that disagrees with
-- its own declaration fails. A host that gains peermem flips one field and this
-- assertion starts demanding device residency without being edited.
--
-- 65535 is kUnknownDevice (core/vocab.hpp), ambiguous on purpose: it means host
-- memory, CUDA-not-present and call-failed at once. PCOMM_TRACE_HOOKS=1 prints
-- the stage/rc/type that separates them, and that is the tool to reach for when
-- a row here surprises you — not a change to this file.
--
-- Names no rank, no address, no size and no count.
WITH reg AS (
  SELECT m.rank, m.base, m.size, CAST(m.device AS INTEGER) AS device,
         max(CAST(r.size AS INTEGER)) OVER (PARTITION BY r.rank) AS biggest,
         min(CAST(r.size AS INTEGER)) OVER (PARTITION BY r.rank) AS smallest
  FROM ib_mem_region r
  JOIN mem_object m ON m.rank = r.rank AND m.base = r.base AND m.size = r.size
),
cfg AS (SELECT CAST(gpudirect AS INTEGER) AS gpudirect FROM params)

-- GPUDirect declared: the heap is the biggest thing a rank registers and the NIC
-- reads it out of GPU memory. Stated as largest-vs-smallest rather than a byte
-- threshold because 38 MB is a number read off one run, while "the heap is the
-- biggest thing this rank registers" is a property of NVSHMEM.
SELECT 'gpudirect declared, but the largest region is not on a device' AS problem,
       rank, base, size, device
FROM reg
WHERE (SELECT gpudirect FROM cfg) = 1
  AND CAST(size AS INTEGER) = biggest AND device = 65535

UNION ALL

-- The other end of the distribution, and only meaningful under GPUDirect: the
-- bootstrap/control buffers are host allocations, so a device ordinal there
-- would mean attribution is answering a different question than the one asked.
SELECT 'smallest registered region is on a device', rank, base, size, device
FROM reg
WHERE (SELECT gpudirect FROM cfg) = 1
  AND CAST(size AS INTEGER) = smallest AND device != 65535

UNION ALL

-- No GPUDirect declared: nothing the NIC registers may be device-resident. If
-- one is, either the host can do GPUDirect after all and the declaration is
-- stale, or attribution is inventing ordinals. Both are findings, and both were
-- invisible before — this is the half the old assertion could not express.
SELECT 'no gpudirect declared, but a registered region is device-resident',
       rank, base, size, device
FROM reg
WHERE (SELECT gpudirect FROM cfg) = 0 AND device != 65535

ORDER BY 1, 2, 3;
