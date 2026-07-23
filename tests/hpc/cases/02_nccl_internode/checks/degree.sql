-- Topology degree in rank-space: how many distinct peers each rank sends to.
-- Coarser than exact edges (qp_num can collide across HCAs at multinode), but
-- deterministic and a real structural invariant of the pinned ring.
SELECT src_rank, count(*) AS peers, sum(CAST(sends AS INTEGER)) AS sends
FROM comm_edges
GROUP BY src_rank
ORDER BY src_rank;
