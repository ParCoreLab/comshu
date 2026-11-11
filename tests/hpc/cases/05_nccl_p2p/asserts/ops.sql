-- requires: gpu_access params.ranks
-- Under this case's pinning, peer traffic is STORES ONLY. Every rank must
-- record peer stores, and no rank may record a peer load.
--
-- Why that is a derivation and not an observation. NCCL moves data over NVLink
-- P2P by *pushing*: the sending CTAs issue stores straight into the receiver's
-- mapped buffer, then store again to a flag telling the consumer the chunk is
-- ready. Writes are posted — fire-and-forget, so the sender never stalls and
-- the link saturates — whereas a remote read is non-posted and costs a full
-- round trip, stalling the warp. Data plane and synchronisation are therefore
-- both stores, and the consumer polls its flag in memory it owns, which is a
-- *local* load and never reaches this table (RemoteFilter keeps only
-- run_device != mem_device).
--
-- SCOPE, and it is the point. This is not a universal claim about NCCL: the P2P
-- transport also has a direct-read mode, and which one is used depends on
-- topology and configuration. The assertion holds because job.sbatch pins the
-- configuration — NCCL_ALGO=Ring, NCCL_PROTO=Simple, MIN/MAX_NCHANNELS=1,
-- intra-node NVLink. If that pinning changes this SHOULD go red, and the answer
-- is to re-derive it, not to relax it.
--
-- The superseded claim, kept because it is instructive: this used to assert that
-- every rank does peer loads AND stores, blaming "NCCL's LL protocol" for
-- unrepeatable counts (295,044 vs 550,064 loads on rank 0). The case pins
-- Simple, not LL — and the loads were never real. They were local reads wearing
-- a phantom device id, from the NVSHMEM positional guess being applied to
-- NCCL's cuMemMap regions. The same comment recorded stores as "a perfectly
-- flat 294,990" across those runs: the stable half was the real half, and the
-- unrepeatability was the bug rather than a property of NCCL. Fixed in
-- 21f3b74 / ebaaf3c; measured 0 loads / 294,987 stores per rank, uniform across
-- all four (job 44139911).
--
-- Counts are still not asserted: NCCL chunks the 262144-element buffer as it
-- sees fit, so volume does not follow from the workload. peer_traffic (golden)
-- is the regression net for that.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'load')  AS peer_loads,
         (SELECT count(*) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store') AS peer_stores
  FROM expected
)
WHERE peer_stores = 0     -- a rank that pushed nothing lost its instrumentation
   OR peer_loads  > 0     -- a peer read means the pinning changed, or attribution regressed

UNION ALL

-- And each rank's stores reach ANOTHER GPU's memory: that is what a p2p send
-- is. (Replaces the peer_traffic golden.)
SELECT r, -1, 0 FROM expected
WHERE NOT EXISTS (
    SELECT 1 FROM gpu_access g
    WHERE CAST(g.rank AS INTEGER) = r AND g.run_dev != g.mem_dev AND g.op = 'store');
