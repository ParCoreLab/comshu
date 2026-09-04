-- requires: comm_edges edge_sizes
-- ... and the lane carries AT LEAST the peer traffic params predicts. The
-- invariant in copy_edges_match_record holds even when both sides are zero,
-- which is exactly what a capture with no copy rows looks like; this is the
-- half that has to come from the workload.
--
-- A FLOOR rather than an equality, deliberately. The phase-5 probe issues a
-- cross-device transfer spelled as a LOCAL copy, and whether CUPTI reports it
-- with one device ordinal or two is the thing this case measures rather than
-- asserts — an equality here would turn a measurement into a claim about
-- whatever CUPTI happened to do, which is a golden in disguise. The floor is
-- true under either answer.
--
-- edge_sizes is in the requires line for the mechanical reason
-- copy_edges_match_record explains at length.
-- CAST TO BIGINT FIRST, and this is not defensive typing. `params` is built
-- from meta.json's integers, which DuckDB binds as INT32, and this product is
-- 200 * 4 * 3 * 4194304 = 10066329600 — measured on du04 as
-- `Out of Range Error: Overflow in multiplication of INT32`, i.e. the assertion
-- ERRORED rather than passing or failing. sqlite computes it in 64 bits and had
-- no complaint, so the two engines disagreed about whether the query was even
-- legal. Any params arithmetic that multiplies a count by a byte size needs
-- this; the widening is on the first factor so it propagates through the rest.
SELECT 'copy lane is short of the peer traffic the workload performed' AS problem,
       (SELECT coalesce(sum(bytes), 0) FROM comm_edges WHERE transport = 'cudamemcpy') AS lane,
       (SELECT CAST(iters AS BIGINT) * gpus * (gpus - 1) * msg_bytes FROM params) AS expected_floor
WHERE lane < expected_floor;
