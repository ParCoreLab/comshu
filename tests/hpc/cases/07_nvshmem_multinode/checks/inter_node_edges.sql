-- FUTURE ORACLE. Rank-space IB topology: only the ring's two node-crossing hops
-- should appear as IB edges. NB the known caveat (things-to-circle-back-to.md):
-- comm_edges' qp_num->rank mapping is approximate at multinode until local-LID
-- capture lands, so this may need degree-only form (as case 02 uses) rather than
-- exact src/dst pairs.
SELECT src_rank, dst_rank, sends, bytes
FROM comm_edges
ORDER BY CAST(src_rank AS INTEGER), CAST(dst_rank AS INTEGER);
