-- requires: gpu_access
-- EXPECTED TO FAIL — same bug and same plan as 04_nvshmem's file of this name,
-- and it must be asserted here separately rather than inherited: this case's
-- whole job is to prove that loading the ib provider alongside gpumem changes
-- nothing about what gpumem captures. If 04 ever goes green and this stays red,
-- coexistence is the difference.
--
-- put_to_next's only peer access is the store into the neighbour's symmetric
-- heap; `src[i]` reads the PE's own buffer, and gpu_access is peer-only
-- (engine.hpp's RemoteFilter drops run_device == mem_device). Measured here:
-- 196608 loads and 196608 stores per rank, byte-identical to case 04 — the
-- fragmented NVSHMEM 3.7 heap makes the local read look remote, in both cases,
-- which at least says the second provider is not what causes it.
SELECT op, count(*) AS accesses, count(DISTINCT rank) AS ranks
FROM gpu_access
WHERE op <> 'store'
GROUP BY op
ORDER BY op;
