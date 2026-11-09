-- requires: mlx5_devx_qp params.ranks
-- Every send in the RoCE DC leg names the rank it went to: the put-ring's
-- neighbour, and nobody else.
--
-- A DC send's peer is the owner of the DCT its address vector names. On
-- InfiniBand that owner is found by (LID, DCT). On RoCE the AV has no LID — its
-- rlid is the UDP source port (60060 and 60325 here) — so the owner is found by
-- (GID, DCT): the GID decoded from the full AV, matched against the GID each
-- DCT recorded at RTR (_owner_gid). Before that, MN5 job 48818283's RoCE leg
-- sent its 20,515 + 20,512 sends to -1, while the IB DC leg resolved every one.
--
-- Read from the CSV comm_edges (CsvAgg); comm_edges_csv_matches_derived holds
-- it equal to the derived table, so this covers both implementations.
--
-- Violations: an unresolved or self edge, then a rank with no edge to its
-- neighbour. Two PEs on one node, so the neighbour is the other rank.
SELECT CAST(src_rank AS INTEGER) AS src_rank,
       CAST(dst_rank AS INTEGER) AS dst_rank,
       CAST(sends AS BIGINT)     AS sends
FROM comm_edges
WHERE CAST(dst_rank AS INTEGER) = -1
   OR CAST(dst_rank AS INTEGER) = CAST(src_rank AS INTEGER)

UNION ALL

SELECT r.rank, -2, 0
FROM ranks_seen r
WHERE NOT EXISTS (
    SELECT 1 FROM comm_edges e
    WHERE CAST(e.src_rank AS INTEGER) = r.rank
      AND CAST(e.dst_rank AS INTEGER) = (r.rank + 1) % (SELECT ranks FROM params)
);
