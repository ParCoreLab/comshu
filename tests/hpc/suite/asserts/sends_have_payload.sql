-- requires: ib_post_send
-- A recorded send must carry a length and at least one sge. Zero-length sends
-- are the shape a half-captured work request takes: `length` and `num_sge` are
-- set by ibv_wr_set_sge, a *different* hooked call from the one that sets the
-- opcode, so a sequencing bug in the batch builder produces rows that exist and
-- say nothing. 08_ib_send_bw exists because the whole table was missing; this
-- guards the next failure along, where the rows come back but empty.
--
-- write-with-immediate is excluded deliberately, not defensively: its payload
-- IS the immediate value, so length = 0 is correct there and 02_nccl_internode
-- legitimately posts 1120 such sends per rank.
--
-- Names no rank or opcode value beyond the imm exclusion.
SELECT rank, opcode, count(*) AS n
FROM ib_post_send
WHERE opcode NOT LIKE '%_imm'
  AND (CAST(length AS INTEGER) <= 0 OR CAST(num_sge AS INTEGER) < 1)
GROUP BY rank, opcode
ORDER BY CAST(rank AS INTEGER), opcode;
