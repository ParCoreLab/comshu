-- Every copy names a CONTEXT device, and it is a GPU this run had.
--
-- `ctx_device` is the field the derived layer takes as ownership evidence — the
-- device whose context issued the copy — so an unset or out-of-range value here
-- does not produce a wrong edge, it produces NO edge: the whole copy matrix
-- falls to rank -1. Measured on du04 before the field existed, with four MPI
-- ranks whose device-space matrix was perfect and whose comm_edges was a single
-- `-1 -> -1` row.
--
-- Unlike src_device and dst_device it is NEVER 65535 on a CUPTI row: a copy is
-- always issued in some context, even when one of its ends is host memory. That
-- makes this the one device column with no sentinel case, which is why it is
-- checked separately from devices_in_range rather than folded into it.
SELECT ctx_device, count(*) AS n
FROM cuda_memcpy
WHERE CAST(ctx_device AS INTEGER) >= (SELECT gpus FROM params)
GROUP BY ctx_device;
