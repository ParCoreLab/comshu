-- requires: ib_post_send params.ranks
-- An all-reduce is symmetric: every rank does the same work. NCCL chooses
-- channels, protocol and chunking, so the byte VOLUME is not derivable from the
-- command line — pinning Ring/Simple/1-channel already moved sends from 57280
-- to 26880 — but the symmetry is, and it is the property that fails when the
-- capture loses a rank.
--
-- Asserted as "every rank posted, and posted as many as the average". No rank
-- id and no send count appears; the rank count comes from the workload, and if
-- all ranks are equal each of them equals total/ranks exactly. A rank whose
-- proxy thread's sends were dropped shows up here as an outlier instead of as a
-- smaller number in a golden nobody can independently justify.
--
-- Peer COUNT is deliberately not asserted, and that is a limitation worth
-- recording rather than papering over: comm_edges maps sends to a destination
-- rank via qp_num, which is only unique per HCA, and at 8 ranks over 2 nodes
-- the mapping collapses. Job 44113361's edges name only dst_rank 0 and 3, with
-- per-rank degrees alternating 1 and 2 — while the per-rank send TOTALS are
-- 3360 for all eight. The degree golden's flat "2" came from a luckier run.
-- Fix is local-LID capture; see things-to-circle-back-to.md.
SELECT * FROM (
  SELECT 'wrong number of senders' AS problem, NULL AS rank,
         (SELECT count(DISTINCT rank) FROM ib_post_send) AS sends
  WHERE (SELECT count(DISTINCT rank) FROM ib_post_send)
        != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'asymmetric', CAST(rank AS INTEGER), count(*)
  FROM ib_post_send
  GROUP BY rank
  HAVING count(*) != (SELECT count(*) FROM ib_post_send) / (SELECT ranks FROM params)
)
ORDER BY problem, rank;
