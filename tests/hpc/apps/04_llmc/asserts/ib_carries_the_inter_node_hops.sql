-- requires: ib_post_send params.nodes params.ranks params.gpus
-- With more than one node, the collective must reach InfiniBand — and the
-- capture must show the intent and the wire in the same run.
--
-- WHAT THIS DELIBERATELY DOES NOT CLAIM. Nothing here derives an IB byte count
-- from `elems`. Case 02's workload_note is the standing reason: NCCL chooses
-- algorithm, protocol, channel count and chunk size, so the wire volume is
-- NCCL's decision and not llm.c's command line. The 30,357,504 elements per step
-- that the collective assertions check are LOGICAL bytes — what the application
-- handed over — and a ring all-reduce moves about 2(P-1)/P of that per rank,
-- through however many posts NCCL decides to use. Asserting a send count here
-- would be inventing a measurement, exactly as nccl_records.hpp says.
--
-- WHAT IT DOES CLAIM, and each of these follows from the topology alone:
--
--   * With nodes > 1 and NVLink P2P left enabled, the ranks are not all mutually
--     NVLink-reachable, so at least one rank must post on IB. Zero IB sends in an
--     8-rank 2-node NCCL run would be a claim-(4) finding: the wire is invisible.
--   * NOTHING about WHICH ranks post. The pre-registered prediction guessed that
--     only the ranks owning a cross-node ring hop would, as in
--     07_nvshmem_multinode; job 45082117 shows all eight posting, because NCCL
--     builds several rings and gives every local GPU an inter-node connection.
--     The prediction was wrong and the assertion never depended on it — which is
--     the difference between the two, and the reason this clause says nothing.
--   * A zero-length send must be a write-with-immediate. This clause was WRONG
--     when this file was first written — it demanded payload on every send, and
--     job 45082117 produced 11,000 offending rows against a capture that was
--     entirely correct. NCCL's inter-node path posts `rdma_write_imm` whose
--     payload IS the 32-bit immediate, so length 0 is the right value there and
--     nowhere else; `suite/asserts/sends_have_payload.sql` already carries the
--     same exclusion and cites 02_nccl_internode posting 1120 such sends a rank.
--     Restated rather than deleted, because the claim that survives is a real
--     one: a zero-length `rdma_write` carries no information at all, so one of
--     those would be a decode error in the length field. In this capture,
--     rdma_write ranges 64..75776 B and never reaches 0.
--   * Every rank that posted on IB is a rank that joined the NCCL communicator.
--     This is the intent-to-wire link the run exists to exhibit: two providers,
--     one process, one execution, and the rank spaces agree.
--
-- Names no rank, no QP, no LID and no length.
SELECT * FROM (
  SELECT 'multi-node NCCL run recorded no IB sends at all' AS problem,
         NULL AS rank, NULL AS detail
  WHERE (SELECT nodes FROM params) > 1
    AND (SELECT count(*) FROM ib_post_send) = 0

  UNION ALL
  SELECT 'zero-length send that is not a write-with-immediate: ' || opcode,
         CAST(rank AS INTEGER), CAST(length AS INTEGER)
  FROM ib_post_send
  WHERE CAST(length AS INTEGER) = 0 AND opcode NOT LIKE '%_imm'

  UNION ALL
  -- The ranks that post on IB must be a subset of the ranks in the communicator.
  -- If a rank appears on the wire and not in nccl_comm the two providers are
  -- numbering ranks differently, and every cross-layer query in the capture is
  -- joining the wrong rows.
  SELECT 'a rank posted on IB but recorded no NCCL communicator',
         CAST(rank AS INTEGER), NULL
  FROM (SELECT DISTINCT rank FROM ib_post_send)
  WHERE rank NOT IN (SELECT rank FROM nccl_comm)
)
ORDER BY problem, rank;
