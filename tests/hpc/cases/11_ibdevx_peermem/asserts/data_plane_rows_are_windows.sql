-- requires: mlx5_device_send
-- Every row the send-queue watcher emitted is what its submitter says it is.
--
-- A Window, not an Instant. The watcher never sees a post; it sees the doorbell
-- record move between two looks at a queue, and records exactly that bound: the
-- WQE was published after `ticks` and before `ticks_hi` (Mlx5DeviceSend's
-- comment in providers/mlx5dv/mlx5_records.hpp). An inverted or zero bound is a
-- time the row does not actually know. Measured on du04 before this was
-- asserted: 41,023 rows, none inverted, median width 3.1 us.
--
-- And fully decoded. Every WQE this workload builds — RDMA writes and the
-- masked atomics NVSHMEM signals with — has a layout the decoder models, on RC
-- (du04) and DC (MN5) alike, so `partial`, `truncated` or `unknown_op` here
-- would be a WQE read from the wrong offset, or a transport that changed shape.
--
-- Emptiness is not this assertion's business — data_plane_puts_arrived.sql and
-- data_plane_covered.sql fail on it — so it may require the table it reads. An
-- invariant that names no rank, queue pair or device.
SELECT rank,
       qp_num,
       opcode_name,
       decode_name,
       count(*) AS n
FROM mlx5_device_send
WHERE submitter_name = 'sq_watch'
  AND (CAST(ticks AS INTEGER) = 0
       OR CAST(ticks_hi AS INTEGER) <= CAST(ticks AS INTEGER)
       OR decode_name != 'full')
GROUP BY rank, qp_num, opcode_name, decode_name
ORDER BY CAST(rank AS INTEGER), qp_num;
