-- requires: comm_edges params.iters params.msg_bytes
-- A pingpong's communication graph is a permutation: every rank sends to
-- exactly one peer, and the whole of its traffic goes there. Asserted as
-- fan-out plus volume, in rank space, naming nobody.
--
-- And the peer is a REAL other rank: never itself, never unresolved (-1).
-- This was left out while comm_edges keyed a peer on qp_num alone, which the
-- fabric hands out per HCA: jobs 44111484 and 44113361 gave both ranks the same
-- qp_num and rank 0's edge collapsed onto itself. Peers are now keyed on
-- (lid, qp_num) — or (gid, qp_num) where LIDs are 0 — which is unique, so a
-- self or -1 edge here is a defect in the tool, not a fabric coincidence. Which
-- other rank is still not named: with two ranks there is only one.
SELECT src_rank,
       count(*)                        AS peers,
       sum(CAST(sends AS INTEGER))     AS sends,
       sum(CAST(bytes AS INTEGER))     AS bytes
FROM comm_edges
GROUP BY src_rank
HAVING peers != 1
    OR sends != (SELECT iters FROM params)
    OR bytes != (SELECT iters * msg_bytes FROM params)

UNION ALL

SELECT CAST(src_rank AS INTEGER), -1, CAST(sends AS INTEGER), CAST(bytes AS INTEGER)
FROM comm_edges
WHERE CAST(dst_rank AS INTEGER) = -1
   OR CAST(dst_rank AS INTEGER) = CAST(src_rank AS INTEGER);
