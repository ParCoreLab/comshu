-- requires: mlx5_device_send params.elem_bytes
-- A data-moving send must carry a payload whose size is consistent with the
-- workload. put-ring moves ints with nvshmem_int_p, so every RDMA write the GPU
-- coalesces is a whole number of elements: length > 0 and a multiple of
-- elem_bytes. A zero length, or one not divisible by the element size, is the
-- shape of a WQE decoded from the wrong segment offset — exactly the bug that
-- produced length 0x08080808 (opcode bytes) and 0x01010101 (payload bytes)
-- before the half-AV / inline-flag decode landed.
--
-- Atomics (atomic_masked_cs/fa) are excluded deliberately, but NOT for the
-- reason this comment used to give. It said "they carry a remote address, not a
-- byte-count payload, so length 0 is correct there", and that was a description
-- of a decoder limitation dressed up as a property of the protocol. An atomic
-- does carry a local data segment — it is simply the LAST segment rather than
-- the one after raddr, because operand segments sit in between. Since the
-- decoder reads it from the end, atomics have real lengths (8 on MN5 job
-- 45582508, a 64-bit signal) and a rule of their own:
-- suite/asserts/atomic_operand_is_a_word.sql.
--
-- They stay out of THIS assertion because its rule is the wrong one for them:
-- an atomic's length is an operand width, not a multiple of the workload's
-- element size. Excluded for a stated reason, not because nothing was read.
SELECT rank, opcode_name, length, count(*) AS n
FROM mlx5_device_send
WHERE opcode_name IN ('rdma_write', 'send', 'rdma_read')
  AND (CAST(length AS INTEGER) <= 0
       OR CAST(length AS INTEGER) % (SELECT elem_bytes FROM params) != 0)
GROUP BY rank, opcode_name, length
ORDER BY CAST(rank AS INTEGER), opcode_name;
