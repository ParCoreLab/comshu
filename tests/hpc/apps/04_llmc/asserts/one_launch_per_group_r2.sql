-- requires: gpu_kernel nccl_group params.iters params.groups_per_iter
-- One NCCL group is one device kernel, and one ungrouped collective is one more.
--
-- ROUND 2. This is the assertion round 1 could not write: it spans the `nccl`
-- provider (what the application asked for) and the `kernel` provider (what the
-- GPU actually ran), and no case in tests/hpc/cases/ asserts anything at all
-- about gpu_kernel — `grep -rn gpu_kernel cases/*/asserts/*.sql` is empty.
--
-- The claim is stated twice, by two parties, neither of them a capture:
--   * llm.c, zero.cuh:530 — "NCCL group: aggregate all pointers in a single
--     NCCL GPU kernel";
--   * comshu.fbs, type 906 — "Group boundaries: N grouped sends are ONE launch,
--     and nothing in a send row says so".
-- So, per rank:
--
--     ncclDevKernel launches = (nccl_group rows)/2 + (collectives outside groups)
--
-- and the second term is exactly the float32 collectives, because all four f32
-- call sites in llm.c sit outside any group (zero.cuh:483, :588,
-- train_gpt2.cu:958, :1022) while every bf16 collective sits inside one
-- (zero.cuh:530-551, train_gpt2.cu:1110-1118). That is a fact about the
-- application's source, not about the capture.
--
-- WHAT IT CATCHES, and they are different defects:
--   * fewer kernels than groups — the kernel provider dropped launches, or its
--     rows are submission records that the link excluded;
--   * more kernels than groups — either the group brackets are not what
--     nccl_group says they are, or NCCL's planner split a fused group. The
--     second is a fact about NCCL, not a comshu defect, and it is named here in
--     advance so that the distinction is not invented afterwards;
--   * zero resolvable kernel names — the vacuous pass. An assertion over an
--     export whose `name` column did not resolve matches no row and reports
--     clean, which is the failure mode this suite has hit twice.
--
-- REPRESENTATION. Run this over a `comshu export --parquet` directory (the only
-- kind `comshu link` accepts). There `gpu_kernel.name` is an intern handle and
-- `nccl_collective.datatype` is a number; in a --csv export both are already
-- text. Both are handled: the name is COALESCEd through intern_text and the
-- datatype is compared as a string against both spellings.
--
-- Names no rank, no device and no stream. `iters` and `groups_per_iter` come
-- from params; the arithmetic comes from llm.c.
WITH kn AS (
  SELECT k.rank,
         COALESCE((SELECT t.text FROM intern_text t
                   WHERE t.rank = k.rank AND t.domain = 3
                     AND CAST(t.handle AS VARCHAR) = CAST(k.name AS VARCHAR)),
                  CAST(k.name AS VARCHAR)) AS kname
  FROM gpu_kernel k
),
nk AS (SELECT rank, kname FROM kn WHERE kname LIKE '%ncclDevKernel%'),
f32 AS (SELECT rank, count(*) AS n FROM nccl_collective
        WHERE CAST(datatype AS VARCHAR) IN ('7', 'float32') GROUP BY rank),
grp AS (SELECT rank, count(*) / 2 AS n FROM nccl_group GROUP BY rank)
SELECT * FROM (
  SELECT 'no kernel name resolved to an NCCL device kernel — vacuous pass' AS problem,
         NULL AS rank, (SELECT count(*) FROM kn) AS got, NULL AS expected
  WHERE NOT EXISTS (SELECT 1 FROM nk)

  UNION ALL
  SELECT 'launches != groups + ungrouped collectives',
         CAST(g.rank AS INTEGER),
         (SELECT count(*) FROM nk WHERE nk.rank = g.rank),
         g.n + COALESCE(f.n, 0)
  FROM grp g LEFT JOIN f32 f ON f.rank = g.rank
  WHERE (SELECT count(*) FROM nk WHERE nk.rank = g.rank) != g.n + COALESCE(f.n, 0)

  UNION ALL
  -- The stage-0 branch pins the grouped half against params rather than against
  -- nccl_group: at -z 0 the backward pass takes groups_per_iter groups per step
  -- and gpt2_update takes none, so the bf16 launches are iters*groups_per_iter
  -- exactly. Selected by the absence of reduce_scatter, which is the one bit of
  -- the capture that can only be the -z flag.
  SELECT 'stage-0 run: wrong number of gradient kernels',
         CAST(rank AS INTEGER), count(*),
         (SELECT iters * groups_per_iter FROM params)
  FROM nk
  WHERE kname LIKE '%bf16%'
    AND NOT EXISTS (SELECT 1 FROM nccl_collective
                    WHERE CAST(op AS VARCHAR) IN ('3', 'reduce_scatter'))
  GROUP BY rank
  HAVING count(*) != (SELECT iters * groups_per_iter FROM params)
)
ORDER BY problem, rank;
