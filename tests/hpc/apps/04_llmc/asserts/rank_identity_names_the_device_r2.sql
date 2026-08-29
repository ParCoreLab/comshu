-- requires: rank_identity nccl_comm params.ranks params.nodes
-- The derived rank identity must name the GPU the rank ran on.
--
-- THIS ASSERTION FAILS TODAY, on every capture in this study, and the failure
-- is the finding — see REPORT-r2.md §3. It is written the way the suite says to
-- write one (suite/README.md, "Never edit an assertion to match what we emit"):
-- it states what the column contract promises, and it goes red until the link
-- delivers it.
--
-- The contract, from apps/BRIEF-BLACKBOX.md:
--
--     rank_identity   rank,node_ix,node_hash,device,devices,label
--
-- `device` is the only reason a consumer reaches for this table: a device
-- ordinal is node-local, so "rank 5 was GPU 1 on node 2" is a fact no single
-- record carries and the derived layer exists to assemble. pcomm.fbs's
-- NodeIdentity comment says so in as many words — "resolving a peer device to a
-- rank needs (node, device), and this is the node half".
--
-- llm.c fixes the right answer: cudaSetDevice(local_device_idx) once per rank
-- (zero.cuh:448), never changed, and ncclCommInitRank records the same ordinal
-- as nccl_comm.cuda_dev (zero.cuh:449). So the device half is in the export,
-- twice — nccl_comm.cuda_dev and gpu_kernel.device both give 0,1,2,3 for ranks
-- 0..3 — while rank_identity.device is NULL and its label reads "n0 · GPU ?".
--
-- WHAT IT CATCHES: exactly that. A derived identity that knows the node and not
-- the device is half a key, and it is silently half: nothing in link_health
-- reports it (`link_health` shows 18 findings, all n = 0, on the same export).
-- A consumer joining on rank_identity.device gets no rows and no error.
--
-- Names no rank, no device and no hash; every comparison is against params or
-- against another column of the same capture.
SELECT * FROM (
  SELECT 'rank_identity does not have one row per rank' AS problem,
         NULL AS rank, CAST((SELECT count(*) FROM rank_identity) AS VARCHAR) AS detail
  WHERE (SELECT count(*) FROM rank_identity) != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'wrong number of distinct nodes', NULL,
         CAST((SELECT count(DISTINCT node_hash) FROM rank_identity) AS VARCHAR)
  WHERE (SELECT count(DISTINCT node_hash) FROM rank_identity) != (SELECT nodes FROM params)

  UNION ALL
  SELECT 'rank_identity names no device though the capture names one',
         CAST(r.rank AS INTEGER),
         'nccl_comm.cuda_dev = ' || CAST(c.cuda_dev AS VARCHAR) || ', rank_identity.device is null'
  FROM rank_identity r JOIN nccl_comm c ON c.rank = r.rank
  WHERE r.device IS NULL OR CAST(r.device AS VARCHAR) = ''

  UNION ALL
  SELECT 'rank_identity.device disagrees with the communicator device',
         CAST(r.rank AS INTEGER),
         CAST(r.device AS VARCHAR) || ' vs ' || CAST(c.cuda_dev AS VARCHAR)
  FROM rank_identity r JOIN nccl_comm c ON c.rank = r.rank
  WHERE r.device IS NOT NULL AND CAST(r.device AS VARCHAR) != ''
    AND CAST(r.device AS INTEGER) != CAST(c.cuda_dev AS INTEGER)
)
ORDER BY problem, rank;
