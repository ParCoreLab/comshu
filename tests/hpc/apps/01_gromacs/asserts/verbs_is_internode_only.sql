-- requires: comm_edges params.ranks params.nodes
-- InfiniBand carries only what crosses a node boundary.
--
-- ORACLE, and it needs nothing from GROMACS: `mpirun --map-by ppr:4:node` puts
-- ranks 0..3 on the first node and 4..7 on the second, so node(r) = r /
-- (ranks/nodes). UCX selects a transport per peer and never puts an intra-node
-- peer on the RC/DC verbs path when shared memory and CUDA IPC are available,
-- which they are on an MN5 accelerated node. So every edge the verbs provider
-- witnesses must join two ranks on DIFFERENT nodes.
--
-- This is an invariant, not a volume: it names no rank, no QP and no LID, only
-- the block-placement rule that the launcher's own arguments fix.
--
-- IT IS ALSO A DELIBERATE PROBE OF A KNOWN WEAKNESS, and if it fails that is
-- the finding rather than a broken claim. suite/README.md: "comm_edges maps
-- QPs->ranks by qp_num, exact at 2 ranks / one HCA, approximate at multinode
-- scale until the local LID is captured directly", and case 01 twice recorded
-- both ranks holding the same qp_num, which collapsed rank 0's edge onto
-- itself. At 8 ranks over 2 nodes with several QPs per peer there is far more
-- room for that collision than at 2. A same-node edge here — especially a
-- self-edge — is that aliasing, not GROMACS sending itself a message.
SELECT 'verbs edge between two ranks the launcher placed on the same node' AS problem,
       CAST(src_rank AS INTEGER) AS src_rank,
       CAST(dst_rank AS INTEGER) AS dst_rank,
       CAST(src_rank AS INTEGER) / ((SELECT ranks FROM params) / (SELECT nodes FROM params)) AS src_node,
       CAST(dst_rank AS INTEGER) / ((SELECT ranks FROM params) / (SELECT nodes FROM params)) AS dst_node,
       sends, bytes
FROM comm_edges
WHERE CAST(src_rank AS INTEGER) >= 0
  AND CAST(dst_rank AS INTEGER) >= 0
  AND CAST(src_rank AS INTEGER) / ((SELECT ranks FROM params) / (SELECT nodes FROM params))
    = CAST(dst_rank AS INTEGER) / ((SELECT ranks FROM params) / (SELECT nodes FROM params))
ORDER BY src_rank, dst_rank;
