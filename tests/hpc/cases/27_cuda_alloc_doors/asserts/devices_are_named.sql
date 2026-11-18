-- requires: mem_object params.size_malloc
-- Every device allocation names its GPU, and every pinned host buffer says it is
-- host memory that CUDA knows — never a failed attribution.
--
-- The VMM rows are the reason this file exists. At cuMemMap time the mapping has
-- no access yet (cuMemSetAccess comes after, as in NCCL and PyTorch), so asking
-- the ADDRESS for its device fails, and the record would read OrdinalFailed — a
-- defect report about a healthy buffer. The engine asks the allocation HANDLE.
--
-- Managed memory may answer either way: CUDA reports its memory type as managed,
-- which mem_attr files under HostCudaKnown, or as device on a platform that
-- migrates it eagerly. Both are honest; a failure is not.
SELECT 'device allocation without a named device' AS problem, origin, size, device, attr_status
FROM mem_object
WHERE origin IN ('CuMemAlloc', 'CuMemAllocAsync', 'CuMemMap')
  AND (attr_status != 'Device' OR CAST(device AS INTEGER) = 65535)
UNION ALL
SELECT 'pinned host buffer not reported as CUDA-known host memory', origin, size, device, attr_status
FROM mem_object
WHERE origin = 'CuMemHost' AND attr_status != 'HostCudaKnown'
UNION ALL
SELECT 'managed buffer attribution failed', origin, size, device, attr_status
FROM mem_object
WHERE origin = 'CuMemAllocManaged' AND attr_status NOT IN ('Device', 'HostCudaKnown')
UNION ALL
-- The floor that keeps the three clauses above from passing on an empty table.
SELECT 'no device allocation recorded at all', NULL, NULL, NULL, NULL
WHERE (SELECT count(*) FROM mem_object WHERE origin = 'CuMemAlloc') = 0;
