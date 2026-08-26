-- requires: nccl_group params.ranks params.iters params.groups_per_iter
-- Every ncclGroupStart in the capture is a fusion site in llm.c's source.
--
-- llm.c brackets its collectives in exactly two places, and neither is nested:
--
--   zero.cuh:530,551          multi_gpu_async_reduce_gradient — ONE pair per call,
--                             taken regardless of ZeRO stage (the branch is inside
--                             the group, not around it). Called once per transformer
--                             layer plus once for the non-block tensors, so L+1 =
--                             groups_per_iter pairs per optimizer step.
--   train_gpt2.cu:1110,1118   gpt2_update — ONE pair per parameter tensor, and ONLY
--                             at zero_stage == 1. NUM_PARAMETER_TENSORS is 16
--                             (train_gpt2.cu:97), a compile-time constant of the
--                             GPT-2 schema, so stage 1 adds 16 pairs per step.
--
-- So per rank: groups_per_iter*iters pairs at stage 0, (groups_per_iter+16)*iters
-- at stage 1, and the branch is selected the same way the algebra assertion
-- selects it — by whether any reduce_scatter was called.
--
-- WHY THIS IS WORTH A FILE OF ITS OWN. The group rows are the only thing in a
-- capture that says which collectives were fused into one launch. Without them
-- a reader counting llm.c's 76 gradient collectives per step reads 76 launches
-- where NCCL performed 7. The count is therefore not decoration: it is the
-- difference between "llm.c issues 76 collectives per step" (true) and "llm.c
-- performs 76 collective launches per step" (false by a factor of eleven).
--
-- Start and End are asserted separately on purpose: an unbalanced pair means a
-- hook fired without its partner, and every launch boundary derived from this
-- table would then be wrong in a way no aggregate count would show.
--
-- Names no rank. 16 is a source constant, not an identifier the run produced.
SELECT * FROM (
  SELECT 'not every rank recorded group boundaries' AS problem, NULL AS rank,
         NULL AS edge, NULL AS got
  WHERE (SELECT count(DISTINCT rank) FROM nccl_group) != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'stage-0 run: wrong group count', CAST(rank AS INTEGER), edge, count(*)
  FROM nccl_group
  WHERE (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') = 0
  GROUP BY rank, edge
  HAVING count(*) != (SELECT groups_per_iter * iters FROM params)

  UNION ALL
  SELECT 'stage-1 run: wrong group count', CAST(rank AS INTEGER), edge, count(*)
  FROM nccl_group
  WHERE (SELECT count(*) FROM nccl_collective WHERE op = 'reduce_scatter') > 0
  GROUP BY rank, edge
  HAVING count(*) != (SELECT (groups_per_iter + 16) * iters FROM params)

  UNION ALL
  -- llm.c never nests a group inside another, so every boundary is outermost and
  -- every End is a launch. A depth above 1 would mean either llm.c nests (it does
  -- not) or the per-thread depth counter is leaking across calls.
  SELECT 'group boundary is nested, llm.c never nests', CAST(rank AS INTEGER),
         edge, CAST(depth AS INTEGER)
  FROM nccl_group WHERE CAST(depth AS INTEGER) != 1
)
ORDER BY problem, rank, edge;
