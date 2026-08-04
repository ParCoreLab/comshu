-- requires: mlx5_devx_cmd mlx5_devx_qp
-- Every DEVX command the provider recognised must have produced the typed
-- record it claims to produce. This is the assertion that makes "decoded" mean
-- something rather than being a flag nobody checks.
--
-- Two ways the decoder can lie, both caught here:
--
--   1. A CREATE_QP (0x500) command counted but no mlx5_devx_qp row emitted —
--      the decode silently failed or returned early. SigmaTrace's version of
--      this code returns early whenever the work-queue buffer is not recognised
--      as GPU memory, which on a dmabuf-registered stack is *always*, and the
--      result is a capture with queue-pair commands and no queue pairs. It does
--      not report anything; it just has fewer rows than it should.
--
--   2. A command marked decoded=1 for an opcode this build does not actually
--      decode — the flag set optimistically.
--
-- Deliberately not asserting the absolute number of CREATE_QPs: NVSHMEM picks
-- how many queue pairs to build from its own heuristics, and pinning that
-- number would be a golden in SQL clothing.
SELECT 'create_qp commands without a decoded queue pair' AS problem,
       rank, n_cmd AS n_create_qp_cmds, n_qp AS n_devx_qp_rows
FROM (
  SELECT c.rank AS rank,
         (SELECT count(*) FROM mlx5_devx_cmd d
           WHERE d.rank = c.rank AND d.opcode = '0x500') AS n_cmd,
         (SELECT count(*) FROM mlx5_devx_qp q WHERE q.rank = c.rank) AS n_qp
  FROM (SELECT DISTINCT rank FROM mlx5_devx_cmd) c
)
WHERE n_cmd != n_qp

UNION ALL

-- A command claiming to be decoded whose opcode is not one this provider
-- decodes at all. The set is closed on purpose: extending the decoder means
-- extending this list, so the assertion cannot silently accept a new claim.
SELECT 'decoded=1 on an opcode the provider does not decode', rank, opcode, decoded
FROM mlx5_devx_cmd
WHERE decoded = 1 AND opcode NOT IN ('0x500', '0x503');
