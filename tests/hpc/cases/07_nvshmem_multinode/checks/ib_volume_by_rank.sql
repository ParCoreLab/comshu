-- Which ranks carry the ring's inter-node traffic, in rank space.
--
-- Aggregated over destinations on purpose. comm_edges' qp_num->rank mapping is
-- approximate at multinode until local-LID capture lands (see
-- things-to-circle-back-to.md), so a bootstrap-sized edge can merge into the
-- bulk edge and shift BOTH the dst_rank attribution and the distinct-peer
-- count. Measured across four captures in three allocations: the per-rank
-- totals below are byte-identical every time, while `dst_rank` splits and
-- `count(*) AS peers` are not — so this case cannot use case 02's degree form
-- either.
--
-- The invariant that survives is the one the case is actually about: exactly
-- two ranks (PE3 -> PE4 and PE7 -> PE0, the ring's node-crossing hops) carry
-- ~196,690 sends, and the other six carry only bootstrap-level traffic.
SELECT src_rank,
       sum(CAST(sends AS INTEGER)) AS sends,
       sum(CAST(bytes AS INTEGER)) AS bytes
FROM comm_edges
GROUP BY src_rank
ORDER BY CAST(src_rank AS INTEGER);
