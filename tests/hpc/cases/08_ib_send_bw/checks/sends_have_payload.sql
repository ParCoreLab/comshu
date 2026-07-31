-- Every recorded send carries the message size and exactly one sge, and there
-- are some. The row count is in the output on purpose: a query that only counts
-- violations passes on an empty table, which is the failure this case exists to
-- catch — the first run of this check reported [ok] against zero captured
-- sends. A check that cannot fail on missing data is not a check.
--
-- Oracle: `ib_send_bw -n 1000 -s 8192` posts 1000 single-sge sends of 8192 B.
-- length and num_sge are set by ibv_wr_set_sge, a *different* hooked call from
-- the one that sets the opcode, so a sequencing bug in the batch builder shows
-- up here as zeros rather than as missing rows.
SELECT count(*) AS sends,
       sum(CASE WHEN CAST(length AS INTEGER) <> 8192 THEN 1 ELSE 0 END) AS wrong_length,
       sum(CASE WHEN CAST(num_sge AS INTEGER) <> 1 THEN 1 ELSE 0 END) AS wrong_num_sge
FROM ib_post_send;
