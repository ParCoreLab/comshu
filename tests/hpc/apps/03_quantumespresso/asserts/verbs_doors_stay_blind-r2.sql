-- requires: ib_device ib_post_send
-- The control. On the default UCX transport the verbs SEND door must stay
-- empty, and the verbs CONTROL plane must not.
--
-- This asserts a NEGATIVE that round 1 measured, so that the day it stops being
-- true somebody notices. Round 1, job 45088258: ib_post_send = 0 rows on the
-- default transport and 18 700 773 rows once UCX was forced onto rc_verbs. The
-- mechanism is UCX's, not comshu's -- rc_mlx5 builds its WQEs and rings the
-- doorbell inline, below every interposable verbs symbol -- and comshu.fbs's
-- ucx_deep header says the same thing from the other side: "On the HPC-X/mlx5
-- stack this is the ONLY layer of comshu that sees the data plane at all:
-- ib_post_send and mlx5_device_send are both empty there."
--
-- WHY BOTH CLAUSES. Either alone is satisfied by a broken capture.
--
--  * ib_post_send non-empty would mean the transport changed under us, which
--    invalidates nothing about ucx_deep but does invalidate the reason this
--    study loads ucx_deep at all. It is worth failing over rather than quietly
--    inheriting.
--  * ib_device EMPTY would mean the ibverbs provider never engaged in this
--    three-provider pass -- and then the first clause is passing for the wrong
--    reason, which is exactly the shape 22_ucx_rma's the_shared_pass_captured_
--    both.sql exists to catch. UCX registers its memory through ibverbs, so the
--    control plane fills even where the data plane does not; ib_device is the
--    cheapest witness of that.
--
-- Together they say: three hook-based providers co-ran in one pass, ibverbs saw
-- the control plane, and the data plane went where round 1 said it went.
--
-- Names no rank, no device and no count. Written without a bare HAVING (an
-- aggregate with no GROUP BY behaves differently across engines) so it means
-- the same thing under sqlite3 and under DuckDB.
SELECT 'the verbs send door fired on the default UCX transport' AS problem,
       (SELECT count(*) FROM ib_post_send) AS ib_post_send_rows
WHERE EXISTS (SELECT 1 FROM ib_post_send)

UNION ALL

SELECT 'the ibverbs provider recorded no device in a three-provider pass', 0
WHERE NOT EXISTS (SELECT 1 FROM ib_device);
