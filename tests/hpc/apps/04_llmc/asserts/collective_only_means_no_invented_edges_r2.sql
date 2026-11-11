-- requires: comm_edges nccl_collective params.p2p_per_iter
-- A workload that names no peer must not produce a rank-to-rank edge.
--
-- ROUND 2. llm.c calls ncclSend and ncclRecv exactly zero times
-- (`grep -c 'ncclSend\|ncclRecv' train_gpt2.cu llmc/*.cuh` = 0, and
-- params.p2p_per_iter = 0 records that), and this study runs `-p nccl,kernel`
-- only: no ibverbs, no UCX, no NVBit. comshu.fbs type 904 says NcclP2p is "the
-- only NCCL call that names a peer, and so the only one an exact edge follows
-- from". So there is nothing in this capture from which a src->dst pair can
-- honestly be derived, and comm_edges must be empty.
--
-- IT IS A CLAIM ABOUT FABRICATION, not about coverage. NCCL owns the algorithm,
-- the ring or tree shape, the channel count and the chunking; none of them is
-- visible to a host-API interposer. An edge appearing here would mean the link
-- had guessed a topology from a collective — the same class of error as case
-- 04's goldens ratifying a phantom device, and worse than the blank table,
-- because a blank table is honestly blank.
--
-- The blank table is itself this study's claim (4) — see REPORT-r2.md §4: the
-- derived view the suite treats as *the* topology table has nothing to say
-- about 243 MB per rank of real NVLink traffic. That is a missing door, named.
--
-- IF THE LINK EVER GAINS A COLLECTIVE LANE, this assertion must be rewritten to
-- check that lane's arithmetic — not deleted, and not relaxed. Its failing
-- would then mean "a new lane exists and nobody stated what it claims".
--
-- Names no rank: the only constant is params.p2p_per_iter.
SELECT * FROM (
  SELECT 'a rank-to-rank edge exists though the workload names no peer' AS problem,
         CAST(src_rank AS VARCHAR) || '->' || CAST(dst_rank AS VARCHAR) AS detail,
         CAST(transport AS VARCHAR) AS transport,
         CAST(sends AS VARCHAR) AS sends,
         CAST(bytes AS VARCHAR) AS bytes
  FROM comm_edges
  WHERE (SELECT p2p_per_iter FROM params) = 0

  UNION ALL
  -- The other half: the capture must also hold no point-to-point call.
  --
  -- WRITTEN AS A PRESENCE TEST, NOT `SELECT count(*) FROM nccl_p2p`, and the
  -- reason is a difference between the two exporters that this study measured:
  -- `comshu export --csv` writes a header-only file for EVERY known record type,
  -- so nccl_p2p.csv exists with zero rows; `comshu export --parquet` materialises
  -- a raw table only when it has rows, so `FROM nccl_p2p` is a hard Catalog
  -- Error there (an export is a set of DuckDB VIEWS over the parquet files, so
  -- duckdb_views() is where a presence test has to look; duckdb_tables() is
  -- empty) — and the derived tables behave the third way, being written out
  -- empty (comm_edges above is a real, empty table in the same export).
  -- Since `comshu link` accepts only a Parquet export, an assertion over the
  -- derived layer has to survive on the representation where empty raw tables
  -- are absent. On Parquet, "nccl_p2p exists" and "llm.c called ncclSend" are
  -- the same statement.
  SELECT 'a point-to-point call was recorded though llm.c makes none',
         'nccl_p2p was materialised', NULL, NULL, NULL
  WHERE (SELECT p2p_per_iter FROM params) = 0
    AND (SELECT count(*) FROM duckdb_views() WHERE view_name = 'nccl_p2p') > 0
)
ORDER BY problem, detail;
