-- requires: ib_post_send comm_edges params.ranks
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
-- The same holds for bytes and for peers, and they are asserted too: this file
-- replaced three goldens (completions, degree, traffic) whose numbers came from
-- one run and could not be justified independently, while their SHAPE — every
-- rank alike — can. Peer count was once left out because comm_edges keyed a
-- peer on qp_num alone and collapsed at 8 ranks (job 44113361: degrees
-- alternating 1 and 2); peers are keyed on (lid, qp_num) now, so every rank
-- must have the same number of peers, none of them itself and none -1. Which
-- peers NCCL's rings pick is not derivable, and is not asserted.
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

  UNION ALL
  SELECT 'asymmetric bytes', CAST(rank AS INTEGER), sum(CAST(length AS BIGINT))
  FROM ib_post_send
  GROUP BY rank
  HAVING sum(CAST(length AS BIGINT))
         != (SELECT sum(CAST(length AS BIGINT)) FROM ib_post_send) / (SELECT ranks FROM params)

  UNION ALL
  SELECT 'unresolved or self edge', CAST(src_rank AS INTEGER), CAST(sends AS BIGINT)
  FROM comm_edges
  WHERE CAST(dst_rank AS INTEGER) = -1 OR CAST(dst_rank AS INTEGER) = CAST(src_rank AS INTEGER)

  UNION ALL
  SELECT 'asymmetric degree', CAST(src_rank AS INTEGER), count(*)
  FROM comm_edges
  GROUP BY src_rank
  HAVING count(*) != (SELECT max(d) FROM (SELECT count(*) AS d FROM comm_edges GROUP BY src_rank))
)
ORDER BY problem, rank;
