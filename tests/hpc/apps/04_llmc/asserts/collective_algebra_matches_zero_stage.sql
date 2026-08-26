-- requires: nccl_collective params.ranks params.iters params.elems params.colls_per_iter
-- The gradient collectives are exactly the ones llm.c's ZeRO branch says they are.
--
-- THIS IS THE CASE. Everything below is read out of llmc/zero.cuh before the
-- capture existed; nothing in it was measured.
--
-- multi_gpu_async_reduce_gradient (zero.cuh:514-552) is a two-way branch with no
-- third arm:
--
--     if (zero_stage == 0)  ncclAllReduce   (ptr, ptr,       size,   ncclFloatX, ncclAvg)
--     else if (== 1)        ncclReduceScatter(ptr, ptr+off,  size/P, ncclFloatX, ncclAvg)
--
-- and at stage 1 gpt2_update adds the other half of the algebra
-- (train_gpt2.cu:1108-1119):
--
--                           ncclAllGather   (shard, whole,   size/P, ncclFloatX)
--
-- So for `iters` optimizer steps, per rank, over `elems` gradient elements:
--
--   stage 0   all_reduce       colls_per_iter*iters rows, sum(count) = elems*iters
--             reduce_scatter   none
--             all_gather       none
--
--   stage 1   reduce_scatter   colls_per_iter*iters rows, sum(count) = elems*iters/ranks
--             all_gather       colls_per_iter*iters rows, sum(count) = elems*iters/ranks
--             bf16 all_reduce  none
--
-- The all_gather arithmetic uses `elems` as the model's total parameter count.
-- That identity holds because llm.c reduces T*C of the position embedding
-- (train_gpt2.cu:963) while allocating max_seq_len*C of it, and the workload
-- pins -t 1024 = max_seq_len. It is stated here because it is an assumption of
-- the workload, not a property of NCCL.
--
-- WHY BOTH BRANCHES IN ONE FILE, and why that is stronger than pinning a stage:
-- the assertion says the capture must be one of exactly two fully specified
-- shapes and nothing else. A capture that is neither — a lost rank, a hook that
-- fired twice, a datatype decoded wrong, an all_gather leaking into a stage-0
-- run — produces an offending row saying which number it actually had. The
-- branch is selected by whether reduce_scatter rows exist at all, which is the
-- one bit of the capture that cannot be anything but the flag: at stage 0 the
-- call is not compiled out, it is simply never reached.
--
-- Names no rank, no stream, no buffer and no count. `ranks`, `iters`, `elems`
-- and `colls_per_iter` all come from params; the datatype names come from
-- ncclFloatX's definition (zero.cuh:27-33), not from the capture.
SELECT * FROM (
  SELECT 'not every rank recorded collectives' AS problem, NULL AS rank, NULL AS got
  WHERE (SELECT count(DISTINCT rank) FROM nccl_collective) != (SELECT ranks FROM params)

  UNION ALL
  -- The gradient traffic is bfloat16 by construction (ncclFloatX). Anything else
  -- at this width means the datatype was decoded from the wrong byte.
  SELECT 'gradient collective is not bfloat16', CAST(rank AS INTEGER), datatype
  FROM nccl_collective
  WHERE op IN ('reduce_scatter', 'all_gather')
    AND datatype != 'bfloat16'

  UNION ALL
  -- ---- stage 0 branch: plain data-parallel all-reduce -----------------------
  SELECT 'stage-0 run: wrong bfloat16 all_reduce count', CAST(rank AS INTEGER), count(*)
  FROM nccl_collective
  WHERE datatype = 'bfloat16'
    AND (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') = 0
  GROUP BY rank
  HAVING count(*) != (SELECT colls_per_iter * iters FROM params)

  UNION ALL
  SELECT 'stage-0 run: wrong reduced element total', CAST(rank AS INTEGER),
         sum(CAST(count AS INTEGER))
  FROM nccl_collective
  WHERE datatype = 'bfloat16'
    AND (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') = 0
  GROUP BY rank
  HAVING sum(CAST(count AS INTEGER)) != (SELECT elems * iters FROM params)

  UNION ALL
  SELECT 'stage-0 run: a bfloat16 collective that is not an all_reduce',
         CAST(rank AS INTEGER), NULL
  FROM nccl_collective
  WHERE datatype = 'bfloat16' AND op != 'all_reduce'
    AND (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') = 0

  UNION ALL
  -- ---- stage 1 branch: reduce-scatter + all-gather over 1/ranks of the model -
  SELECT 'stage-1 run: wrong ' || op || ' count', CAST(rank AS INTEGER), count(*)
  FROM nccl_collective
  WHERE op IN ('reduce_scatter', 'all_gather')
    AND (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') > 0
  GROUP BY rank, op
  HAVING count(*) != (SELECT colls_per_iter * iters FROM params)

  UNION ALL
  SELECT 'stage-1 run: wrong ' || op || ' element total', CAST(rank AS INTEGER),
         sum(CAST(count AS INTEGER))
  FROM nccl_collective
  WHERE op IN ('reduce_scatter', 'all_gather')
    AND (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') > 0
  GROUP BY rank, op
  HAVING sum(CAST(count AS INTEGER)) != (SELECT elems * iters / ranks FROM params)

  UNION ALL
  -- Sharding the optimizer state must REMOVE the whole-gradient all-reduce, not
  -- add to it. A bfloat16 all_reduce surviving into a stage-1 run would mean the
  -- flag did not take, and llm.c would be paying for both.
  SELECT 'stage-1 run: a bfloat16 all_reduce survived the ZeRO branch',
         CAST(rank AS INTEGER), count(*)
  FROM nccl_collective
  WHERE datatype = 'bfloat16' AND op = 'all_reduce'
    AND (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') > 0
  GROUP BY rank

  UNION ALL
  -- Nothing else in llm.c reduces. broadcast/reduce/all_to_all appear nowhere in
  -- its source, so a row carrying one is a decode error in the op field.
  SELECT 'collective op llm.c never calls: ' || op, CAST(rank AS INTEGER), NULL
  FROM nccl_collective
  WHERE op NOT IN ('all_reduce', 'reduce_scatter', 'all_gather')
)
ORDER BY problem, rank;
