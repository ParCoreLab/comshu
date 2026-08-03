-- requires: comm_edges params.iters params.msg_bytes
-- A pingpong's communication graph is a permutation: every rank sends to
-- exactly one peer, and the whole of its traffic goes there. Asserted as
-- fan-out plus volume, in rank space, naming nobody.
--
-- *Which* rank the peer is, is deliberately NOT asserted, and that is a
-- statement about our tooling rather than about pingpong. comm_edges maps a
-- send to a destination rank through its qp_num, which is only unique per HCA:
-- in job 44111484 the fabric handed BOTH ranks qp_num 449 and in 44113361 it
-- handed both 461, so rank 0's edge collapsed onto itself (0->0) in each. The
-- topology golden went red twice for a fabric coincidence with nothing wrong in
-- the capture — the failure this file exists to stop. A dst_rank assertion
-- becomes honest once the local LID is captured directly (see
-- things-to-circle-back-to.md); until then, fan-out and volume are the part of
-- the claim the data actually supports.
SELECT src_rank,
       count(*)                        AS peers,
       sum(CAST(sends AS INTEGER))     AS sends,
       sum(CAST(bytes AS INTEGER))     AS bytes
FROM comm_edges
GROUP BY src_rank
HAVING peers != 1
    OR sends != (SELECT iters FROM params)
    OR bytes != (SELECT iters * msg_bytes FROM params)
ORDER BY CAST(src_rank AS INTEGER);
