-- Every device ordinal is either a GPU this run had or the record's host
-- sentinel. 65535 is kUnknownDevice, a schema constant rather than a property
-- of the workload, so it is spelled; the upper bound comes from params.
--
-- The cross-cutting owner_in_range assertion covers gpu_access and cannot see
-- this table, and a device field that is uninitialised rather than absent is
-- the exact defect it was written for — arriving on a new record.
SELECT src_device, dst_device, count(*) AS n
FROM cuda_memcpy
WHERE (CAST(src_device AS INTEGER) != 65535
       AND CAST(src_device AS INTEGER) >= (SELECT gpus FROM params))
   OR (CAST(dst_device AS INTEGER) != 65535
       AND CAST(dst_device AS INTEGER) >= (SELECT gpus FROM params))
GROUP BY src_device, dst_device;
