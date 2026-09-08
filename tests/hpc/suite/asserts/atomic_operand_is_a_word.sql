-- requires: mlx5_device_send
-- An atomic operates on ONE word, so its decoded length must be a word width.
--
-- This is the check that keeps the atomic decode honest. The local data segment
-- of an atomic work-queue entry is not at a fixed offset from the remote
-- address — operand segments sit in between, one for most operations and two
-- for an 8-byte masked compare-swap — so the decoder finds it by counting back
-- from the end of the entry (ds - 1) rather than forward. That is sound, and it
-- is also exactly the kind of derived offset that is wrong silently: read one
-- segment early and `length` becomes an operand value, a mask, or half of a
-- 64-bit compare word, all of which are plausible-looking integers.
--
-- What they are not is word widths. An atomic acts on a single 1-, 2-, 4- or
-- 8-byte quantity by definition of the operation, so anything else in `length`
-- means the data segment was located wrong. NVSHMEM narrows it further —
-- ibgda_write_amo_wqe asserts `bytes == 4 || bytes == 8` and writes exactly that
-- into byte_count — but the wider set is asserted here, because the rule this
-- file wants to state is a property of atomics, not of one transport's use of
-- them.
--
-- Scoped to decode = full. A Partial atomic is one whose segment count did not
-- match the shape the decoder knows, so it never read a data segment and has
-- nothing to be checked; failing it here would punish the decoder for correctly
-- declining to guess.
--
-- Note the interaction with the NIC wire check in case 12, which sums decoded
-- lengths against port_xmit_data. Until atomics had a length they contributed
-- zero to that sum, so atomic traffic was invisible to the one oracle in this
-- suite that is not pcomm checking pcomm. Both halves of that had to change
-- together: a length that is wrong would now make the wire check wrong too,
-- which is why this assertion exists rather than trusting the read.
SELECT rank,
       opcode_name,
       length,
       count(*) AS n
FROM mlx5_device_send
WHERE opcode_name LIKE 'atomic%'
  AND decode_name = 'full'
  AND CAST(length AS INTEGER) NOT IN (1, 2, 4, 8)
GROUP BY rank, opcode_name, length
ORDER BY CAST(rank AS INTEGER), opcode_name
