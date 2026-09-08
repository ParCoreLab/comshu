-- requires: mlx5_device_send
-- One queue pair, one address-vector width. Every WQE, every time.
--
-- The AV width is not a property of the WQE — it is a property of the DEVICE.
-- NVSHMEM sets support_half_av_seg once, from the port, and every queue pair it
-- builds thereafter emits the same form: 16-byte compact (av_ds=1) on an IB port
-- that allows it, 48-byte full (av_ds=3) on a RoCE port, where lid == 0 forces
-- a GRH. A queue pair cannot change its mind mid-stream.
--
-- The decoder, however, derives that width SEPARATELY FOR EVERY WQE, by reading
-- MLX5_EXTENDED_UD_AV out of dqp_dct at AV offset 8. That is the right design —
-- nothing has to be plumbed in from the transport, and a capture states the
-- width instead of assuming it — but it means the answer is re-derived from raw
-- bytes thousands of times per queue pair, and every one of those reads is at an
-- offset computed from the PREVIOUS field's width.
--
-- So this assertion is the one thing the decode cannot fake. If the parser ever
-- walks off — a segment sized wrong, a WQEBB stride miscounted, a wrap handled
-- badly — the ext bit gets read from somewhere that is not offset 8, and the
-- derived width starts flickering within a single queue pair. Uniform-but-wrong
-- is possible; varying is not, and varying is exactly what a drifting offset
-- looks like.
--
-- WHY IT IS NOT A TAUTOLOGY, since two neighbouring facts nearly make it one.
-- `is_dci` comes from the queue pair's service type, so "an RC send has no AV"
-- is true by construction and worth nothing as a check. And av_ds is only ever
-- assigned 1 or 3, so its RANGE proves nothing either. What is not constructed
-- is the ext BIT: it is read from the queue's memory, per WQE, at an offset the
-- parser computed. This asserts the only property of that read which the code
-- does not already guarantee — that it agrees with itself.
--
-- Scoped to rows that HAVE an address vector (av_ds > 0), so an RC capture
-- contributes nothing rather than trivially passing. Grouped by rank as well as
-- qp_num because queue-pair numbers are assigned per HCA and two ranks
-- routinely hold the same one — case 01 caught the fabric handing both ends
-- qp_num 449.
--
-- CAST inside the aggregate, not outside. sql_check.sh loads these CSVs into
-- sqlite where every column is TEXT, and count(DISTINCT '1') vs
-- count(DISTINCT 1) is the same trap decode_starts_at_the_queue_head.sql fell
-- into and doorbell_sequence_complete.sql documents.
--
-- First proven exercisable on MN5 job 45580050, which produced av_ds=3 for the
-- first time by binding IBGDA to the acc nodes' Ethernet mlx5 port.
SELECT s.rank,
       s.qp_num,
       count(DISTINCT CAST(s.av_ds AS INTEGER)) AS distinct_widths,
       group_concat(DISTINCT CAST(s.av_ds AS INTEGER)) AS widths_seen,
       count(*) AS n_sends
FROM mlx5_device_send s
WHERE CAST(s.av_ds AS INTEGER) > 0
GROUP BY s.rank, s.qp_num
HAVING count(DISTINCT CAST(s.av_ds AS INTEGER)) > 1
