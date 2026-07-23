-- Per-rank IB send volume by opcode. With NCCL pinned (Ring/Simple, 1 channel)
-- and fixed size/iters this is byte-repeatable. Guards the at-scale IB path.
SELECT rank, opcode, count(*) AS n, sum(CAST(length AS INTEGER)) AS bytes
FROM ib_post_send
GROUP BY rank, opcode
ORDER BY rank, opcode;
