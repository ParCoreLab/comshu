-- requires: ib_post_send params.grad_elems params.iters params.elem_bytes params.nodes
-- The InfiniBand payload must be at least what an all-reduce over four
-- independent node-vectors REQUIRES, whatever algorithm NCCL chose.
--
-- THE BOUND, AND IT IS INFORMATION-THEORETIC RATHER THAN ALGORITHMIC. Per rank
-- the application hands over S = iters * grad_elems * elem_bytes bytes of
-- gradient. Each node can reduce its four GPUs' gradients locally to one vector
-- of S bytes, but the four NODE vectors are mutually independent -- they are
-- sums over disjoint data shards -- so every node must EXPORT at least S bytes
-- for its contribution to influence the result anywhere else. Summed over the
-- four nodes, the total egress is at least nodes * S. Sends are egress, so
--
--     sum(ib_post_send.length) over all ranks  >=  nodes * iters * G * 2
--
-- Nothing about rings, trees, channels, chunking or protocols enters that. It
-- is the same refusal 04_llmc made explicitly ("No IB quantity is asserted
-- against elems, deliberately") turned into the one inequality that CAN be
-- asserted without knowing NCCL's algorithm.
--
-- CALIBRATION, FROM A DIFFERENT RUN AND NOT FROM THIS ONE. 04_llmc's two-node
-- job 45082117 gives the same bound as 2S = 485 720 064 B and measured ~535 MB,
-- i.e. 1.10x. So the bound is tight enough to be informative: a capture that
-- lost half its sends would fall under it.
--
-- WHAT IT CATCHES. A capture that drops ib_post_send rows under sixteen-rank
-- load; a `length` field that means something other than payload bytes; and the
-- specific failure of hooking only some of the send doors, which shows up as a
-- total that is a clean fraction of the bound rather than as an obvious zero.
--
-- WHY NO UPPER BOUND IS ASSERTED. A flat ring over 16 GPUs would put
-- 4 * 2 * (15/16) * S = 1.82 GB on the fabric and a tree or hierarchical
-- algorithm less; which one NCCL picks is its business and llm.c never names it.
-- An upper bound here would be asserting NCCL, not pcomm. The measurement is
-- printed by job.sbatch for the report instead.
--
-- rdma_write_imm CARRIES ITS PAYLOAD IN THE 32-BIT IMMEDIATE and reports
-- length = 0; 04_llmc measured 11 000 such posts per two-node stage and
-- suite/asserts/sends_have_payload.sql already documents the exclusion. That is
-- why the claim is on the SUM and not on a per-post minimum -- a zero-length
-- post is correct here and must not be a violation.
SELECT 'the fabric carried less than an all-reduce over four nodes requires' AS problem,
       total_bytes,
       (SELECT nodes * iters * grad_elems * elem_bytes FROM params) AS bound,
       posts
FROM (SELECT sum(CAST(length AS BIGINT)) AS total_bytes, count(*) AS posts FROM ib_post_send) x
WHERE total_bytes < (SELECT nodes * iters * grad_elems * elem_bytes FROM params)

UNION ALL

SELECT 'ib_post_send is empty, so this assertion checked nothing', 0,
       (SELECT nodes * iters * grad_elems * elem_bytes FROM params), 0
WHERE (SELECT count(*) FROM ib_post_send) = 0;
