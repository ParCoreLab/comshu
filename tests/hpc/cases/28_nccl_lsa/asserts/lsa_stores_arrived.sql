-- requires: gpu_access params.ranks params.iters params.window_bytes params.elem_bytes
-- Every rank's window stores reached the capture, exactly once, and nothing else
-- crossed a GPU.
--
-- lsa_ring's put_ring is the program's ONLY peer access: one volatile 4-byte
-- store per window word per iteration, into the right neighbour's window. The
-- fill-free design (no collective, no device communicator) is what makes the
-- count derivable: window_bytes / elem_bytes * iters stores per rank, every one
-- a store, every one to another GPU. A load would mean a local access wearing a
-- peer's device id — the attribution failure case 05's ops.sql guards against.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT 'wrong store volume' AS problem, r AS rank,
         (SELECT count(*) FROM gpu_access g
          WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store'
            AND g.run_dev != g.mem_dev AND CAST(g.size AS INTEGER) = (SELECT elem_bytes FROM params)) AS n,
         (SELECT window_bytes / elem_bytes * iters FROM params) AS expected
  FROM expected
  WHERE (SELECT count(*) FROM gpu_access g
         WHERE CAST(g.rank AS INTEGER) = r AND g.op = 'store'
           AND g.run_dev != g.mem_dev AND CAST(g.size AS INTEGER) = (SELECT elem_bytes FROM params))
        != (SELECT window_bytes / elem_bytes * iters FROM params)

  UNION ALL
  SELECT 'unexpected access: ' || op || ' of ' || size || ' B', CAST(rank AS INTEGER), count(*), NULL
  FROM gpu_access
  WHERE op <> 'store' OR CAST(size AS INTEGER) <> (SELECT elem_bytes FROM params)
  GROUP BY rank, op, size
)
ORDER BY problem, rank;
