-- requires: ib_post_recv params.ranks params.rx_depth params.msg_bytes
-- Each side pre-posts exactly `rx_depth` receives of `msg_bytes`, and every one
-- of them resolves to the buffer it will land in.
--
-- THE ORACLE IS THE PROGRAM, not a previous capture. ibv_rc_pingpong's
-- pp_init_ctx registers one buffer and posts `rx_depth` receives into it before
-- the loop starts (default -r 500). Inside the loop it reposts only when its
-- outstanding count falls to 1, and with `-n 20` against a depth of 500 it never
-- does — 20 completions take the count from 500 to 480. So the expected number
-- is rx_depth exactly, and that followed from the command line before this row
-- type existed. The measurement then agreed: 500 per rank, both ranks.
--
-- The asymmetry this closes is worth stating plainly. Until ib_post_recv landed
-- this case could assert 20 sends and 20 recv completions per rank and say
-- NOTHING about the 500 buffers those completions landed in — the receive half
-- of a pingpong, the most symmetric workload in the suite, was invisible.
--
-- Driven off the workload's rank list rather than GROUP BY, for the reason
-- traffic.sql spells out: a rank that posted NOTHING forms no group, so a HAVING
-- clause cannot see it. That failure mode is live here — case 08's receiving
-- side posts 1000 receives while its sending side posts none, so "no rows for
-- this rank" is a shape this suite genuinely produces.
--
-- `resolved` is the point of the record rather than a bonus column. addr/lkey
-- are the first sge's, which SigmaTrace does not keep, and joining lkey to
-- ib_mem_region is what turns "a receive was posted" into "a receive was posted
-- INTO THIS REGISTERED BUFFER". If that join ever drops to 0 the row type has
-- lost the only thing that distinguishes it.
WITH RECURSIVE expected(r) AS (
  SELECT 0
  UNION ALL SELECT r + 1 FROM expected WHERE r + 1 < (SELECT ranks FROM params)
)
SELECT * FROM (
  SELECT r AS rank,
         (SELECT count(*) FROM ib_post_recv p
          WHERE CAST(p.rank AS INTEGER) = r AND p.origin = 'qp') AS recvs,
         (SELECT coalesce(sum(CAST(p.length AS INTEGER)), 0) FROM ib_post_recv p
          WHERE CAST(p.rank AS INTEGER) = r AND p.origin = 'qp') AS bytes,
         (SELECT count(*) FROM ib_post_recv p
          JOIN ib_mem_region m
            ON CAST(m.rank AS INTEGER) = CAST(p.rank AS INTEGER)
           AND CAST(m.lkey AS INTEGER) = CAST(p.lkey AS INTEGER)
          WHERE CAST(p.rank AS INTEGER) = r AND p.origin = 'qp') AS resolved
  FROM expected
)
WHERE recvs != (SELECT rx_depth FROM params)
   OR bytes != (SELECT rx_depth * msg_bytes FROM params)
   OR resolved != (SELECT rx_depth FROM params)
ORDER BY rank;
