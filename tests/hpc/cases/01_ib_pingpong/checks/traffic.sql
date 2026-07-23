-- Every posted send, by rank and opcode. Oracle: ibv_rc_pingpong -n 20 posts
-- exactly 20 SENDs of 4096 B per side.
SELECT rank, opcode, count(*) AS n, sum(CAST(length AS INTEGER)) AS bytes
FROM ib_post_send
GROUP BY rank, opcode
ORDER BY rank, opcode;
