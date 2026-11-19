-- requires: ucx_atomic
-- An atomic's operand is a 4- or 8-byte word, on every door that says.
--
-- ucp_atomic_op_nbx passes `count` ELEMENTS, the width living in
-- param->datatype; the provider recorded count itself, so every nbx atomic read
-- as a 1-byte operand (CR-54) — the same "one-byte atomic" shape that, from the
-- workload's side, job 45177421 found the transport refusing. 0 is "the caller
-- did not say" and is allowed; anything else that is not a word is a decode bug.
SELECT door, op, op_size, count(*) AS n
FROM ucx_atomic
WHERE CAST(op_size AS INTEGER) NOT IN (0, 4, 8)
GROUP BY door, op, op_size
