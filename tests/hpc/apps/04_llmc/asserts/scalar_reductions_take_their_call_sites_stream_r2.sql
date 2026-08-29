-- requires: nccl_collective params.iters
-- Each scalar reduction must be recorded on the stream its call site names.
--
-- ROUND 2, and it is a strictly stronger claim than round 1's "two distinct
-- streams per rank", which a provider that stamped every row with a constant
-- pair of handles would also satisfy. llm.c has exactly two streams
-- (zero.cuh:450 nccl_stream, train_gpt2.cu:1183 main_stream) and four float32
-- call sites, and the source says which stream each one uses:
--
--     zero.cuh:483        multi_gpu_barrier          nccl_stream
--     zero.cuh:588        multi_gpu_cpu_float_sum    nccl_stream
--     train_gpt2.cu:958   final loss reduction       main_stream
--     train_gpt2.cu:1022  gradient norm (ZeRO-1)     main_stream
--
-- The nccl_stream sites fire 3 times in this workload and not once per step:
-- once from gpt2_allocate_state (train_gpt2.cu:412) and once per validation,
-- and validation runs at step 0 and at the last step only because -v 1000 is
-- larger than iters (train_gpt2.cu:1718, :1727). Three is therefore a constant
-- of the *command line*, not of the run — the same kind of oracle as the 4 in
-- scalar_reduction_count_is_sizeof_float.sql, which is sizeof(float).
--
-- The main_stream sites fire once per optimizer step each: `iters` at -z 0
-- (loss only) and 2*iters at -z 1 (loss + grad norm, train_gpt2.cu:1003-1022
-- being reachable only when the gradient was reduce-scattered).
--
-- WHAT IT CATCHES: a `stream` field read from the wrong argument slot, or
-- stamped from a per-communicator default instead of per call — both of which
-- leave the distinct-stream count at 2 and put the collectives on the wrong
-- side of it. It also fails if the gradient collectives ever leave nccl_stream,
-- which would mean llm.c's compute/communication overlap had silently stopped.
--
-- Names no stream handle. Handles are per-rank intern ids — measured 3 on every
-- rank and 77 on rank 0 against 149 on ranks 1-3 for the very same
-- CUstream — so any assertion naming one would be a golden. Everything below is
-- relative: "the stream that carries the gradient collectives" and "not that
-- one".
WITH grad_stream AS (
  SELECT rank, min(stream) AS s FROM nccl_collective
  WHERE CAST(datatype AS VARCHAR) IN ('9', 'bfloat16') GROUP BY rank
),
f32 AS (
  SELECT c.rank, c.stream, count(*) AS n,
         max(CASE WHEN CAST(c.stream AS VARCHAR) = CAST(g.s AS VARCHAR) THEN 1 ELSE 0 END) AS on_grad
  FROM nccl_collective c JOIN grad_stream g ON g.rank = c.rank
  WHERE CAST(c.datatype AS VARCHAR) IN ('7', 'float32')
  GROUP BY c.rank, c.stream
)
SELECT * FROM (
  SELECT 'gradient collectives are spread over more than one stream' AS problem,
         CAST(rank AS INTEGER) AS rank, CAST(count(DISTINCT stream) AS VARCHAR) AS got,
         '1' AS expected
  FROM nccl_collective
  WHERE CAST(datatype AS VARCHAR) IN ('9', 'bfloat16')
  GROUP BY rank HAVING count(DISTINCT stream) != 1

  UNION ALL
  SELECT 'wrong number of scalar reductions on the gradient stream',
         CAST(rank AS INTEGER), CAST(n AS VARCHAR), '3'
  FROM f32 WHERE on_grad = 1 AND n != 3

  UNION ALL
  SELECT 'no scalar reduction on the gradient stream at all',
         CAST(g.rank AS INTEGER), '0', '3'
  FROM grad_stream g
  WHERE NOT EXISTS (SELECT 1 FROM f32 WHERE f32.rank = g.rank AND on_grad = 1)

  UNION ALL
  SELECT 'scalar reductions off the gradient stream use more than one stream',
         CAST(rank AS INTEGER), CAST(count(*) AS VARCHAR), '1'
  FROM f32 WHERE on_grad = 0 GROUP BY rank HAVING count(*) != 1

  UNION ALL
  -- iters at stage 0 (the loss), 2*iters at stage 1 (loss + grad norm), and the
  -- stage is read off the capture by whether reduce_scatter exists at all.
  SELECT 'wrong number of per-step scalar reductions on the compute stream',
         CAST(rank AS INTEGER), CAST(n AS VARCHAR),
         CAST((SELECT CASE WHEN EXISTS (SELECT 1 FROM nccl_collective
                                        WHERE CAST(op AS VARCHAR) IN ('3','reduce_scatter'))
                           THEN 2 * iters ELSE iters END FROM params) AS VARCHAR)
  FROM f32
  WHERE on_grad = 0
    AND n != (SELECT CASE WHEN EXISTS (SELECT 1 FROM nccl_collective
                                       WHERE CAST(op AS VARCHAR) IN ('3','reduce_scatter'))
                          THEN 2 * iters ELSE iters END FROM params)
)
ORDER BY problem, rank;
