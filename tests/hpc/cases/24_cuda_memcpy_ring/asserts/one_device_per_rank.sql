-- requires: rank_identity
-- Each rank drives exactly ONE GPU, and the capture can say which.
--
-- `devices` is the count of distinct devices the reader found evidence for on
-- that rank; `device` is NULL unless there is exactly one. Before ctx_device
-- existed this table reported `devices = 0` for every rank of this workload —
-- the capture had no evidence at all — which is the state that made the whole
-- matrix unresolvable. Derived: the expected count is 1 because the workload
-- calls cudaSetDevice once per rank, and no ordinal is named.
SELECT rank, device, devices FROM rank_identity
WHERE CAST(devices AS INTEGER) != 1 OR device IS NULL;
