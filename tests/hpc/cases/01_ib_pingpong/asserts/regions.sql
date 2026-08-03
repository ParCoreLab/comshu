-- requires: ib_mem_region params.ranks params.regions_per_rank params.msg_bytes
-- ibv_rc_pingpong registers one buffer per endpoint, of the message size. Both
-- numbers are properties of the program (see its pp_init_ctx), not of the run,
-- which is why they are in meta.json's workload block rather than in a golden.
--
-- The count matters more than it looks: `ibv_reg_mr` is a macro that only
-- reaches the legacy exported symbol when the access flags are a compile-time
-- constant, and for months we hooked only that symbol. pingpong takes the
-- literal path and so kept passing while ordinary callers registered invisibly
-- (see 08_ib_send_bw). A region count that drops to zero here is that class of
-- blindness arriving on the path we do cover — which is exactly why the query
-- walks the workload's ranks instead of the table's, so zero is a row and not
-- an absence. See traffic.sql.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM ib_mem_region m
          WHERE CAST(m.rank AS INTEGER) = r) AS regions,
         (SELECT coalesce(sum(CAST(m.size AS INTEGER)), 0) FROM ib_mem_region m
          WHERE CAST(m.rank AS INTEGER) = r) AS bytes
  FROM expected
)
WHERE regions != (SELECT regions_per_rank FROM params)
   OR bytes   != (SELECT regions_per_rank * msg_bytes FROM params)
ORDER BY rank;
