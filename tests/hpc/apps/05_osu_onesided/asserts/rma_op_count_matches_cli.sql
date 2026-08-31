-- requires: ucx_rma params.msg_min_bytes params.ops_per_size
-- THE COUNT ORACLE. Each size in the sweep must appear exactly (warmup+iters)
-- x win_ops times, and that number is on the command line.
--
-- This is the claim the application was chosen for. run_put_with_flush
-- (osu_put_bw.c:305-317) issues window_size MPI_Puts per iteration for
-- skip+iterations iterations, at one size, into one window, from rank 0 alone:
-- 8 x 25 = 200 operations of that size and no others. `-i` sets both
-- iterations and iterations_large and `-x` sets both skip and skip_large
-- (osu_util.c:360, :326), so the large-message re-set at osu_put_bw.c:296
-- cannot change the count halfway through the sweep. Nothing about 200 is
-- observed; it is arithmetic over four arguments in job.sbatch.
--
-- WHAT IT CATCHES. A dropped operation, a double-recorded one, a hook that
-- misses the first call before the interposition settles, or a `count` column
-- that is not what the record implies. It is the only assertion in this study
-- that would notice pcomm losing one row in two hundred — every existence check
-- in the suite passes just as green on 199 rows as on 200.
--
-- WHY THE FILTER IS A SIZE AND NOT AN OP. MPI's own window synchronisation
-- moves 8- and 64-bit words, so a floor of msg_min_bytes = 512 separates the
-- workload's traffic from MPI's exactly. Filtering on `op` instead would be
-- unportable: the RmaOp vocabulary is published nowhere a black-box user may
-- read, and the CSV export decodes the column while the Parquet export does
-- not, so a name matches on one engine and nothing on the other.
--
-- IF THIS FAILS BUT rma_sweep_is_complete PASSES — the multiplicities wrong,
-- the byte total right — the finding is that osc/ucx split one MPI_Put into
-- several ucp_put_nbi calls. That is a fact about OpenMPI and not a defect in
-- pcomm, and PREDICTION.md pre-registers it as such so it cannot be
-- rationalised after the fact. The assertion still stays red.
SELECT 'a message size in the sweep was recorded the wrong number of times' AS problem,
       size                              AS message_bytes,
       n                                 AS rows_recorded,
       (SELECT ops_per_size FROM params) AS rows_expected
FROM (SELECT CAST(count AS INTEGER) AS size, count(*) AS n
      FROM ucx_rma
      WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params)
      GROUP BY 1) s
WHERE n <> (SELECT ops_per_size FROM params);
