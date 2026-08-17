-- requires: nccl_collective nccl_p2p nccl_group params.ranks params.iters
-- Every rank made exactly the calls the command line asked for.
--
-- THIS ARITHMETIC IS ONLY LEGITIMATE AT THIS LAYER, and case 02's workload_note
-- is the contrast: there, elems and iters buy nothing because NCCL picks
-- channels, protocol and chunking, so the wire volume is its decision and not
-- the command line's. Here the provider records the calls the APPLICATION made.
-- No NCCL internal choice can change how many times the application called
-- ncclAllReduce, so `colls_per_iter * iters` is derived, not observed — which
-- is the difference between an assertion and a golden wearing a costume.
--
-- Names no rank and no count: both come from params. A capture that lost a
-- rank, or a hook that fired twice for one call, shows up as an offending row
-- with the number it actually had.
SELECT * FROM (
  SELECT 'not every rank recorded collectives' AS problem, NULL AS rank, NULL AS n
  WHERE (SELECT count(DISTINCT rank) FROM nccl_collective) != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'wrong collective count for this rank', CAST(rank AS INTEGER), count(*)
  FROM nccl_collective GROUP BY rank
  HAVING count(*) != (SELECT colls_per_iter * iters FROM params)

  UNION ALL
  SELECT 'wrong point-to-point count for this rank', CAST(rank AS INTEGER), count(*)
  FROM nccl_p2p GROUP BY rank
  HAVING count(*) != (SELECT p2p_per_iter * iters FROM params)

  -- One Start and one End per iteration, per rank. An unbalanced pair means a
  -- hook fired without its partner, which would silently corrupt every launch
  -- boundary derived from this table.
  UNION ALL
  SELECT 'wrong group ' || edge || ' count for this rank', CAST(rank AS INTEGER), count(*)
  FROM nccl_group GROUP BY rank, edge
  HAVING count(*) != (SELECT groups_per_iter * iters FROM params)
)
ORDER BY problem, rank;
