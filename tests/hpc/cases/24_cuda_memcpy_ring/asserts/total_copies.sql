-- Every copy the workload performed: one per rank per iteration, and nothing
-- else. copybench's phases do not exist here — this workload allocates, hands
-- out an IPC handle and issues peer copies, so `ranks * iters` is the whole
-- capture. A bare count against an arithmetic expression over params, which is
-- the one shape that fails on an empty table.
SELECT * FROM (SELECT count(*) AS copies FROM cuda_memcpy)
WHERE copies != (SELECT ranks * iters FROM params);
