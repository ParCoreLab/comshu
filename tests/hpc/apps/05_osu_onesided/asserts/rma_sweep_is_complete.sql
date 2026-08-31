-- requires: ucx_rma params.msg_min_bytes params.n_sizes params.total_bytes
-- The whole sweep ran, and it moved the byte total the command line asked for.
--
-- TWO CLAUSES, and they fail differently, which is why they are here rather
-- than folded into rma_op_count_matches_cli.
--
--   * A size that produced NO rows at all is invisible to a per-size
--     multiplicity check: that check iterates the sizes it can see. Losing the
--     first window of the sweep entirely — an interposition that engages one
--     window late, say — would leave four perfectly-formed groups of 200 and a
--     green board. Counting the distinct sizes is what notices.
--   * The byte total is the coarser claim and the more robust one. If UCP ever
--     fragments an MPI_Put into several ucp_put_nbi calls the multiplicities
--     break while this holds, and the pair of results tells a reader which of
--     the two happened.
--
-- 3,174,400 B = 200 x (512+1024+2048+4096+8192), from the four arguments in
-- job.sbatch. MPI_CHAR is the benchmark's only datatype (osu_put_bw.c:72), so
-- element counts and byte counts coincide and the comparison is meaningful
-- whichever UcxRma.count is meant to be.
SELECT 'the size sweep is missing sizes' AS problem,
       (SELECT count(DISTINCT CAST(count AS INTEGER)) FROM ucx_rma
        WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params)) AS observed,
       (SELECT n_sizes FROM params)                                         AS expected
WHERE (SELECT count(DISTINCT CAST(count AS INTEGER)) FROM ucx_rma
       WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params))
      <> (SELECT n_sizes FROM params)
UNION ALL
SELECT 'the one-sided byte total does not match the command line',
       (SELECT coalesce(sum(CAST(count AS INTEGER)), 0) FROM ucx_rma
        WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params)),
       (SELECT total_bytes FROM params)
WHERE (SELECT coalesce(sum(CAST(count AS INTEGER)), 0) FROM ucx_rma
       WHERE CAST(count AS INTEGER) >= (SELECT msg_min_bytes FROM params))
      <> (SELECT total_bytes FROM params);
