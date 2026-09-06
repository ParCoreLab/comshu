-- requires: mlx5_device_send mlx5_devx_qp params.ranks
-- The decode must start at the BEGINNING of a queue pair it watched being made.
--
-- A DEVX queue pair is created with its producer index at zero. This provider
-- registers it from the CREATE_QP decode, before it can have sent anything, so
-- every WQE it ever posts is observable and the first one sits at producer
-- index 0. If the smallest wqe_idx decoded for such a queue pair is not 0, the
-- copy-out began partway through the queue and whatever preceded it is missing
-- from the capture.
--
-- THIS IS NOT HYPOTHETICAL. on_doorbell used to anchor the copy-out at the
-- producer index the FIRST doorbell announced and return without copying, which
-- discarded exactly the WQEs that doorbell was announcing — one send per queue
-- pair, on every capture, since the decode was written. MN5 job 45497029 shows
-- the shape it left: doorbells = sends + 1 on every active queue pair.
--
-- WHY NOTHING ELSE CAUGHT IT, which is the argument for this file existing.
-- unknown_op and truncated were zero, because every row that WAS emitted
-- decoded cleanly. wqes_lapped was zero, because nothing lapped. sends_are_sized
-- passed, because the surviving rows had the right sizes. The application's
-- output was byte-identical, because the instrument does not change what the
-- workload sends. And the NIC wire check passed, because under-reporting is the
-- direction a one-sided upper bound cannot see. Every check agreed, and the
-- capture was still missing sends.
--
-- Scoped to gpu_driven queue pairs: those are the ones whose WQEs this provider
-- copies out. A CPU-side DEVX queue pair is recorded but never decoded, so it
-- has no sends and nothing to say here.
--
-- Caveat worth stating rather than discovering: wqe_idx is the 16-bit producer
-- index and wraps. A queue pair that posts more than 65,536 WQEs can legitimately
-- report a nonzero minimum, so this assertion belongs to a bounded workload —
-- which case 12's is, at 20 iterations.
-- CAST is not decoration, and it goes INSIDE the min(). sql_check.sh loads
-- these CSVs into sqlite, where every column is TEXT — the same point
-- doorbell_sequence_complete.sql makes, and which the first version of this
-- file ignored twice over. min() over TEXT is lexicographic, so a queue that
-- posted 9 and 10 would report 10 as its smallest index; and `'0' != 0`
-- compares TEXT against INTEGER, which sqlite's type affinity makes TRUE. The
-- result was an assertion that fired on all four queue pairs of a capture that
-- was correct — a false alarm on the very fix it was written to protect, which
-- is worse than not having written it.
SELECT s.rank, s.qp_num, min(CAST(s.wqe_idx AS INTEGER)) AS first_wqe_decoded,
       count(*) AS n_sends
FROM mlx5_device_send s
JOIN mlx5_devx_qp q ON q.rank = s.rank AND q.qp_num = s.qp_num
WHERE CAST(q.gpu_driven AS INTEGER) = 1
GROUP BY s.rank, s.qp_num
HAVING min(CAST(s.wqe_idx AS INTEGER)) != 0
