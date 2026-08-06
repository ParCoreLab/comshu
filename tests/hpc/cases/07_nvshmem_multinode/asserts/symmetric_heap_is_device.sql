-- requires: ib_mem_region mem_object
-- The registered symmetric heap is device memory, and the capture now says so.
--
-- This is what the plan's step 3 buys. NVSHMEM's inter-node path moves the
-- symmetric heap by GPUDirect RDMA: the NIC reads GPU memory directly, so the
-- largest region each rank registers is device-resident by construction of the
-- transport, not by observation of a run. Before mem_object existed on this
-- path, ib_mem_region carried mr/pd/range/lkey/rkey/access and nothing that
-- could distinguish that heap from a host bounce buffer.
--
-- Stated as largest-vs-smallest rather than against a byte threshold, for the
-- same reason ib_carries_the_crossing states a separation rather than a cutoff:
-- 38 MB is a number read off a run, "the heap is the biggest thing this rank
-- registers" is a property of NVSHMEM. No count is asserted either — case 06
-- measured 115 regions on one rack and 114 on another at the same commit.
--
-- 65535 is kUnknownDevice (core/vocab.hpp), and it is ambiguous on purpose: it
-- means host memory, CUDA-not-present and call-failed all at once. That is why
-- the smallest-region clause is a SEPARATE row with its own label — if both
-- fire, attribution is dead rather than merely wrong, and PCOMM_TRACE_HOOKS=1
-- prints the stage/rc/type that says which.
--
-- Names no rank, no address, no size and no count.
SELECT 'largest registered region is not on a device' AS problem, rank, base, size, device
FROM (
  SELECT m.rank, m.base, m.size, m.device,
         max(CAST(r.size AS INTEGER)) OVER (PARTITION BY r.rank) AS biggest
  FROM ib_mem_region r
  JOIN mem_object m ON m.rank = r.rank AND m.base = r.base AND m.size = r.size
)
WHERE CAST(size AS INTEGER) = biggest AND CAST(device AS INTEGER) = 65535

UNION ALL

-- The other end of the distribution: the bootstrap/control buffers are host
-- allocations, so a device ordinal there would mean the attribution is
-- answering something other than the question asked.
SELECT 'smallest registered region is on a device', rank, base, size, device
FROM (
  SELECT m.rank, m.base, m.size, m.device,
         min(CAST(r.size AS INTEGER)) OVER (PARTITION BY r.rank) AS smallest
  FROM ib_mem_region r
  JOIN mem_object m ON m.rank = r.rank AND m.base = r.base AND m.size = r.size
)
WHERE CAST(size AS INTEGER) = smallest AND CAST(device AS INTEGER) != 65535

ORDER BY 1, 2, 3;
