-- Op kinds captured per rank. Deliberately NOT counts: NCCL chunks the buffer
-- as it sees fit, so volume does not follow from the workload (peer_traffic is
-- the regression net for that).
--
-- Stores only. NCCL pushes over NVLink P2P — the sender stores into the peer's
-- mapped buffer and stores again to the ready flag, because writes are posted
-- and a remote read would stall the warp on a round trip. The consumer polls a
-- flag it owns, which is a local load and is filtered out. Scoped to this
-- case's pinning (Ring / Simple / 1 channel, intra-node): NCCL's P2P transport
-- also has a direct-read mode, so this is a property of the configuration, not
-- of NCCL.
--
-- This golden used to carry a `load` row per rank. Those were never real — the
-- NVSHMEM positional guess was being applied to NCCL's cuMemMap regions, giving
-- local reads a phantom device id that survived the peer-only filter. Corrected
-- alongside 21f3b74 / ebaaf3c, derived from the push model rather than read off
-- the run.
SELECT rank, op
FROM gpu_access
GROUP BY rank, op
ORDER BY CAST(rank AS INTEGER), op;
