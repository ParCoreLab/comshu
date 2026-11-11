-- requires: nccl_collective params.ranks
-- llm.c's four scalar reductions pass sizeof(float) where NCCL wants an ELEMENT
-- COUNT, so every one of them reduces 4 floats instead of 1. The capture must
-- show 4, because 4 is what the application asked for.
--
--   zero.cuh:483        multi_gpu_barrier          ncclAllReduce(..., sizeof(float), ncclFloat, ncclSum)
--   zero.cuh:588        multi_gpu_cpu_float_sum    ncclAllReduce(..., sizeof(float), ncclFloat, ncclSum)
--   train_gpt2.cu:958   backward, final loss       ncclAllReduce(..., sizeof(float), ncclFloat, ncclAvg)
--   train_gpt2.cu:1022  grad-norm, ZeRO-1 only     ncclAllReduce(..., sizeof(float), ncclFloat, ncclSum)
--
-- ncclAllReduce's third parameter is `size_t count` in elements (nccl.h), so
-- sizeof(float) = 4 makes each of these a four-element reduction over a
-- one-element quantity. Two of them are harmless-looking — element 0 still gets
-- the right answer and the other three land in slack inside a larger buffer.
-- The one at zero.cuh:588 is not: its buffer is
--
--   cudaMallocManaged(&result.unified_buffer, sizeof(float))   zero.cuh:455
--
-- four bytes of MANAGED memory, and NCCL writes sixteen into it. That is a
-- twelve-byte heap overflow on every barrier and every cross-rank float sum,
-- present at llm.c upstream f1e2ace.
--
-- SO THIS ASSERTION PASSES WHEN comshu IS RIGHT AND llm.c IS WRONG. It is the
-- clean case of an oracle that is not the capture: the number 4 comes from
-- `sizeof(float)` in the application's source, and if the capture had shown 1 —
-- the value a reader would expect from the variable names — the provider would
-- be inventing it. That is why the check is written as an equality against a
-- constant of the C language rather than "the count is small".
--
-- Names no rank and no identifier. 4 is sizeof(float), not something the run
-- produced.
SELECT * FROM (
  SELECT 'scalar reduction is not float32' AS problem, CAST(rank AS INTEGER) AS rank,
         CAST(count AS INTEGER) AS n
  FROM nccl_collective
  WHERE datatype = 'float32' AND op != 'all_reduce'

  UNION ALL
  -- The claim itself.
  SELECT 'float32 all_reduce count is not sizeof(float)', CAST(rank AS INTEGER),
         CAST(count AS INTEGER)
  FROM nccl_collective
  WHERE datatype = 'float32' AND CAST(count AS INTEGER) != 4

  UNION ALL
  -- logical_bytes must be the reader's own count x width and nothing else. If
  -- these ever disagree the derived column has acquired a second definition.
  SELECT 'logical_bytes is not count x elem_bytes', CAST(rank AS INTEGER),
         CAST(logical_bytes AS INTEGER)
  FROM nccl_collective
  WHERE CAST(logical_bytes AS INTEGER)
        != CAST(count AS INTEGER) * CAST(elem_bytes AS INTEGER)

  UNION ALL
  -- Every rank participates in every one of these; they are collective calls on
  -- straight-line code. A rank missing from the float32 set means a lost stream.
  SELECT 'not every rank recorded the scalar reductions', NULL, NULL
  WHERE (SELECT count(DISTINCT rank) FROM nccl_collective WHERE datatype = 'float32')
        != (SELECT ranks FROM params)
)
ORDER BY problem, rank, n;
