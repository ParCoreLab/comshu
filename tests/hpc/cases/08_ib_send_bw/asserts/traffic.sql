-- requires: ib_post_send params.iters params.msg_bytes
-- `ib_send_bw -n 1000 -s 8192` posts 1000 sends of 8192 B from the client. The
-- server only receives, so exactly one rank appears here — asserted as a count
-- of sending ranks rather than as "rank 1", since which endpoint the job script
-- makes the client is not what this case is about.
--
-- The regression this case exists for is the whole table being empty: perftest
-- posts through ibv_wr_start/ibv_wr_send on ibv_qp_ex, which are static inline
-- wrappers over function pointers inside the QP and never reach
-- ctx->ops.post_send. Job 43976008 produced 202,000 completions and ZERO sends,
-- and every check in the suite stayed green. A HAVING clause over an empty
-- table is green too, which is why the "no sender at all" branch is written
-- separately and does not depend on any row existing.
SELECT 'wrong volume' AS problem, rank, count(*) AS sends,
       sum(CAST(length AS INTEGER)) AS bytes
FROM ib_post_send
GROUP BY rank
HAVING sends != (SELECT iters FROM params)
    OR bytes != (SELECT iters * msg_bytes FROM params)

UNION ALL
SELECT 'wrong number of senders', NULL,
       (SELECT count(DISTINCT rank) FROM ib_post_send), NULL
WHERE (SELECT count(DISTINCT rank) FROM ib_post_send) != 1

ORDER BY 1, 2;
