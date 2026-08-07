-- requires: mlx5_device_send params.elem_bytes
-- A data-moving send must carry a payload whose size is consistent with the
-- workload. put-ring moves ints with nvshmem_int_p, so every RDMA write the GPU
-- coalesces is a whole number of elements: length > 0 and a multiple of
-- elem_bytes. A zero length, or one not divisible by the element size, is the
-- shape of a WQE decoded from the wrong segment offset — exactly the bug that
-- produced length 0x08080808 (opcode bytes) and 0x01010101 (payload bytes)
-- before the half-AV / inline-flag decode landed.
--
-- Atomics (atomic_masked_cs/fa) are excluded deliberately: they carry a remote
-- address, not a byte-count payload, so length 0 is correct there. Reads and
-- writes and sends are the data ops, and this names no rank or address.
SELECT rank, opcode_name, length, count(*) AS n
FROM mlx5_device_send
WHERE opcode_name IN ('rdma_write', 'send', 'rdma_read')
  AND (CAST(length AS INTEGER) <= 0
       OR CAST(length AS INTEGER) % (SELECT elem_bytes FROM params) != 0)
GROUP BY rank, opcode_name, length
ORDER BY CAST(rank AS INTEGER), opcode_name;
