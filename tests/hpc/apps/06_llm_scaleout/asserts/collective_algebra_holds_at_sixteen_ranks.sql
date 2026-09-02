-- requires: nccl_collective params.ranks params.bf16_rows params.bf16_elems params.f32_colls params.f32_count
-- The collective algebra llm.c's source determines, re-checked at P=16.
--
-- THIS IS A CONTROL, NOT THE FINDING. It is here so that everything downstream
-- has a foundation: if the NCCL layer is wrong there is no point asking whether
-- the verbs layer agrees with it. Every number was derived from llm.c before it
-- ran at P=4 and P=8 (04_llmc/PREDICTION.md) and matched exactly at both, so a
-- mismatch here is about the sixteen-rank run and not about the derivation.
--
-- THE ORACLE, ALL OF IT FROM llm.c:
--   * bf16_rows: 76 gradient collectives per optimizer step -- twelve per layer
--     over L = 6 layers, plus four, from the nelem[] list at
--     train_gpt2.cu:938-945 and :963 -- times 4 steps. At ZeRO stage 1 each site
--     becomes a reduce-scatter AND an all-gather (zero.cuh:543 plus
--     train_gpt2.cu:1113), so the row count doubles while the volume halves.
--   * bf16_elems: at stage 0 it is iters * G with
--     G = L(12C^2 + 13C) + Vp.C + T.C + 2C = 30 357 504 for -e d6 -t 1024, and
--     it is P-INDEPENDENT because every rank all-reduces the whole gradient.
--     llm.c prints `num_parameters: 30357504` itself, which is the check on the
--     arithmetic. At stage 1 it is 2 * iters * G / P, i.e. TWO ops over shards
--     of G/16 = 1 897 344 -- an integer only because C = 384 = 16 x 24 makes
--     every one of the sixteen parameter tensors divisible by sixteen. THAT is
--     the number that is new at this scale: 15 178 752 against 30 357 504 at
--     P=8 and 60 715 008 at P=4.
--   * the f32 rows are the ungrouped scalar reductions -- zero.cuh:483, :588,
--     train_gpt2.cu:958, and at stage 1 also :1022 -- and every one passes
--     `sizeof(float)` where ncclAllReduce wants an ELEMENT COUNT, so count = 4.
--     That is a defect in llm.c and this assertion expects 4. Do not "fix" it to
--     1: suite/README.md calls editing an assertion to match what the tool
--     emitted the one inversion that destroys a test, and here the number 4 is
--     an oracle from the C language rather than from the capture.
--
-- THE f32 ROWS ARE SEPARATED FROM THE bf16 ONES BY THEIR COUNT, not by their
-- datatype column, because the datatype enum's vocabulary is not published
-- anywhere a black-box reader can see (it is `uint8` in pcomm.fbs with no
-- declared enumerators) and because the CSV and Parquet exports disagree about
-- whether such a field is decoded to a name or left as an integer. count = 4 is
-- an unambiguous discriminator here: no gradient tensor in this model has four
-- elements -- the smallest is C = 384.
--
-- WHAT IT CATCHES: a provider that drops rows under sixteen-way load; a count
-- field that is right at four ranks and wrong at sixteen; and a build that
-- silently lost -DMULTI_GPU. That last is not hypothetical -- llm.c's Makefile
-- gates MULTI_GPU on `dpkg -l | grep nccl`, MN5 has no dpkg, and 04_llmc's
-- NOTES.md records that `make` there produces a single-GPU binary that calls no
-- NCCL at all and looks exactly like a provider that failed to hook.
SELECT 'a rank did not record the expected number of bf16 collectives' AS problem,
       rank, n, (SELECT bf16_rows FROM params) AS want
FROM (SELECT rank, count(*) AS n FROM nccl_collective
      WHERE CAST(count AS BIGINT) <> (SELECT f32_count FROM params) GROUP BY rank) x
WHERE n <> (SELECT bf16_rows FROM params)

UNION ALL

SELECT 'a rank did not record the expected number of scalar f32 collectives',
       rank, n, (SELECT f32_colls FROM params)
FROM (SELECT rank, count(*) AS n FROM nccl_collective
      WHERE CAST(count AS BIGINT) = (SELECT f32_count FROM params) GROUP BY rank) x
WHERE n <> (SELECT f32_colls FROM params)

UNION ALL

SELECT 'not every rank appears in nccl_collective',
       -1, (SELECT count(DISTINCT rank) FROM nccl_collective), (SELECT ranks FROM params)
WHERE (SELECT count(DISTINCT rank) FROM nccl_collective) <> (SELECT ranks FROM params)

UNION ALL

-- The strongest single number: the gradient element total per rank.
SELECT 'a rank moved the wrong number of gradient elements',
       rank, elems, (SELECT bf16_elems FROM params)
FROM (SELECT rank, sum(CAST(count AS BIGINT)) AS elems FROM nccl_collective
      WHERE CAST(count AS BIGINT) <> (SELECT f32_count FROM params) GROUP BY rank) x
WHERE elems <> (SELECT bf16_elems FROM params);
