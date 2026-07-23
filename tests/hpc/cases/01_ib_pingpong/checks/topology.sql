-- Rank-space communication graph (run-id-free). Oracle: the two ranks send to
-- each other, 20 sends / 81920 B each way.
SELECT src_rank, dst_rank, sends, bytes
FROM comm_edges
ORDER BY src_rank, dst_rank;
