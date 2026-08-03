-- requires: ib_post_send params.msg_bytes
-- Every individual send carries the message size and exactly one sge — not just
-- the right total, which a run that posted half the sends at twice the size
-- would also produce.
--
-- length and num_sge are set by ibv_wr_set_sge, a *different* hooked call from
-- ibv_wr_send which sets the opcode. The two have to be stitched together into
-- one record by the batch builder, so a sequencing bug there shows up as rows
-- that exist and are wrong rather than as rows that are missing — invisible to
-- traffic.sql's totals if the errors happen to cancel, and visible here per
-- work request.
SELECT length, num_sge, count(*) AS n
FROM ib_post_send
WHERE CAST(length  AS INTEGER) != (SELECT msg_bytes FROM params)
   OR CAST(num_sge AS INTEGER) != 1
GROUP BY length, num_sge
ORDER BY 1, 2;
