-- requires: nccl_collective params.ranks params.p2p_per_iter
-- Two shape claims about llm.c that the capture must reproduce exactly.
--
-- (1) NO POINT-TO-POINT. llm.c contains no ncclSend and no ncclRecv:
--
--       grep -c 'ncclSend\|ncclRecv' train_gpt2.cu llmc/*.cuh   ->  0
--
--     Data parallelism needs none — every rank holds the whole model (stage 0)
--     or a strided shard of it (stage 1), and both are expressed entirely in
--     collectives. So `nccl_p2p` must be empty, and params.p2p_per_iter says so
--     rather than the absence of the table saying it. Stated as a claim because
--     an empty table and a table the provider failed to open look identical, and
--     that confusion is what 08_ib_send_bw's empty ib_queue_pair was.
--
-- (2) EXACTLY TWO STREAMS CARRY NCCL WORK. llm.c creates one dedicated
--     communication stream (cudaStreamCreate(&result.nccl_stream), zero.cuh:450)
--     and enqueues on it everything that can overlap with compute — the gradient
--     reduction, the ZeRO-1 all-gather, the barrier, the cross-rank float sum.
--     The two reductions that the host must immediately read enqueue on
--     main_stream instead: the loss (train_gpt2.cu:958, followed by a
--     cudaMemcpyAsync of the result) and the grad norm (train_gpt2.cu:1022,
--     followed by a blocking cudaMemcpy). So every rank must show exactly two
--     distinct stream handles and no more.
--
--     This is the only place in the capture where llm.c's compute/communication
--     overlap strategy is visible at all, and it is a real claim: a third stream
--     would mean a collective escaped the design, and one stream would mean the
--     overlap is not happening.
--
-- Names no stream handle, no rank and no peer. Handles are only ever counted,
-- never compared to a value.
SELECT * FROM (
  SELECT 'llm.c calls no ncclSend/ncclRecv, but nccl_p2p has rows' AS problem,
         NULL AS rank, NULL AS n
  WHERE (SELECT p2p_per_iter FROM params) = 0
    AND (SELECT count(*) FROM nccl_p2p) != 0

  UNION ALL
  SELECT 'rank did not enqueue on exactly two streams', CAST(rank AS INTEGER),
         count(DISTINCT stream)
  FROM nccl_collective GROUP BY rank
  HAVING count(DISTINCT stream) != 2

  UNION ALL
  -- Every collective llm.c makes is on the one communicator, so a rank showing
  -- more than one comm handle in its collectives has either lost a row or
  -- interned two handles for one object.
  SELECT 'rank used more than one communicator handle', CAST(rank AS INTEGER),
         count(DISTINCT comm)
  FROM nccl_collective GROUP BY rank
  HAVING count(DISTINCT comm) != 1

  UNION ALL
  -- No llm.c collective has a root: all_reduce, reduce_scatter and all_gather are
  -- rootless, and the provider writes -1 for those. A root >= 0 would mean a
  -- broadcast or reduce got in, or the field was filled from the wrong argument.
  SELECT 'a rootless collective carries a root', CAST(rank AS INTEGER),
         CAST(root AS INTEGER)
  FROM nccl_collective WHERE CAST(root AS INTEGER) != -1
)
ORDER BY problem, rank;
