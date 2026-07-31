-- The regression this case exists for: perftest's sends must appear at all.
-- Oracle: `ib_send_bw -n 1000 -s 8192` posts 1000 SEND work requests on the
-- client. Before the qp_ex patch this query returned no rows whatsoever, and
-- nothing else in the suite noticed.
SELECT rank, opcode, count(*) AS n, sum(CAST(length AS INTEGER)) AS bytes
FROM ib_post_send
GROUP BY rank, opcode
ORDER BY rank, opcode;
