-- requires: comm_edges
-- NB comm_edges here is the `--csv` export, whose columns are
-- (src_rank, dst_rank, sends, bytes) — no `transport`. That column exists only
-- in the `--link` Parquet. It is not needed: this case loads ib alone with one
-- GPU per node, so there is no NVLink traffic to exclude by construction.
-- The hub answers with less than it receives — slot/8 against slot, per spoke.
--
-- Direction, not ratio. The exact factor is the workload's business; what must
-- hold for the TOOL to be right is that the two directions are not conflated.
-- An aggregate that symmetrised an edge would report the answer flow as a full
-- slot and fail here, and no ring or pair capture in this suite could catch it.
SELECT 'hub does not receive strictly more than it sends' AS problem,
       (SELECT sum(CAST(bytes AS INTEGER)) FROM comm_edges
         WHERE CAST(dst_rank AS INTEGER) = 0) AS into_hub,
       (SELECT sum(CAST(bytes AS INTEGER)) FROM comm_edges
         WHERE CAST(src_rank AS INTEGER) = 0) AS out_of_hub
WHERE (SELECT sum(CAST(bytes AS INTEGER)) FROM comm_edges
        WHERE CAST(dst_rank AS INTEGER) = 0)
   <= (SELECT sum(CAST(bytes AS INTEGER)) FROM comm_edges
        WHERE CAST(src_rank AS INTEGER) = 0);
