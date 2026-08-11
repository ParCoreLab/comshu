-- requires: entity_close
-- Every close names an entity some open record created, and closes never
-- outnumber opens for one subject.
--
-- `subject` is the interned handle the open record carries, and interned handles
-- are unique across providers, so this join is exact and needs no provider
-- knowledge: a memory region, a queue pair, a umem and a UAR are all checked by
-- the same two clauses.
--
-- WHAT AN UNMATCHED CLOSE MEANS. Not a bug in the close hook. It is the mirror
-- of case 08's unaccounted queue pair — a transition referring to a queue pair
-- nothing created — arriving from the other side: evidence of a CREATION door we
-- do not hook. That case took a week to find because the only symptom was an
-- empty table, and six hypotheses died to instruments that answer questions
-- about symbols while the answer was a function pointer.
--
-- So these rows are the cheapest possible detector for the next missing door,
-- and the temptation when one fires will be to filter it out. Don't. Find the
-- door. Suppressing the row discards the only signal that would have found it.
--
-- Scoped by rank throughout. Handles are per-process counters, so handle 3 on
-- rank 0 and handle 3 on rank 1 are unrelated — the same discipline comm_edges
-- learned from qp_num, which is HCA-local and made a qp_num-keyed map resolve
-- every colliding peer to whichever rank was scanned last.
--
-- Names no rank, no handle and no count.
-- `opener` is the type_id of the record that should have created the subject,
-- so an unmatched row names the door to go looking for instead of merely saying
-- something is missing: 100 ib_mem_region, 102 ib_queue_pair,
-- 104 ib_completion_queue, 108 ib_shared_recv_queue, 118 ib_address_handle,
-- 120 ib_device, 200 mlx5_umem, 202 mlx5_uar, 520 mem_object.
-- 104 ib_completion_queue, 108 ib_shared_recv_queue, 118 ib_address_handle and
-- 120 ib_device joined this list when the receive path landed. Every one of them
-- HAD to arrive with its close hook in the same change: an entity type that
-- emits EntityClose rows but is absent from this CTE turns each of its closes
-- into a spurious "names an entity no open record created", and this assertion
-- becomes noise the moment it is most needed. The subject of ib_device is the
-- interned ibv_context*, closed at ibv_close_device.
--
-- 210 mlx5_queue_pair is the case that failure mode was warning about, and it
-- had already happened. A queue pair created through mlx5dv_create_qp gets its
-- open record in the mlx5dv provider's range, while the close comes from
-- ibv_destroy_qp — so every such QP was a guaranteed "close names an entity no
-- open record created" on any workload that both creates through mlx5dv and
-- tears down. It is an open record like any other; leaving it out was an
-- omission from this list, not a check.
-- (As of 2026-08-11 ibv_destroy_qp is interposed by the ENGINE, not ibverbs, so
-- this close is recorded even on a `-p mlx5dv`-only run where ibverbs is absent
-- — which is what makes the IBGDA cases (09/10/12) account their queue pairs at
-- all. The close first carried opener 102 unconditionally, because 102 was all
-- the closing hook could know; the creating providers now register each opened
-- handle with the engine, so a close reports the door that actually made its
-- subject — 210 for an mlx5dv-created queue pair. Either way this CTE joins on
-- subject alone, so `opener` stays a signpost for the reader and a capture
-- written by an older engine still checks out. See
-- PLAN-20260805-0053-entity-lifetimes.)
WITH opens AS (
  SELECT rank, mr AS subject, 100 AS opener FROM ib_mem_region
  UNION ALL SELECT rank, qp, 102 FROM ib_queue_pair
  UNION ALL SELECT rank, cq, 104 FROM ib_completion_queue
  UNION ALL SELECT rank, srq, 108 FROM ib_shared_recv_queue
  UNION ALL SELECT rank, ah, 118 FROM ib_address_handle
  UNION ALL SELECT rank, context, 120 FROM ib_device
  UNION ALL SELECT rank, qp, 210 FROM mlx5_queue_pair
  UNION ALL SELECT rank, umem, 200 FROM mlx5_umem
  UNION ALL SELECT rank, uar, 202 FROM mlx5_uar
  UNION ALL SELECT rank, id, 520 FROM mem_object
),
closes AS (SELECT rank, subject, opener, count(*) AS n
           FROM entity_close GROUP BY rank, subject, opener),
open_n AS (SELECT rank, subject, count(*) AS n FROM opens GROUP BY rank, subject)

SELECT 'close names an entity no open record created (opener ' || c.opener || ')' AS problem,
       c.rank, c.subject, c.n AS closes, 0 AS opens
FROM closes c
WHERE NOT EXISTS (SELECT 1 FROM open_n o WHERE o.rank = c.rank AND o.subject = c.subject)

UNION ALL

-- More closes than opens for one subject. A handle is a pointer value, so it CAN
-- legitimately repeat once the address is reused — but only after a close, which
-- means opens and closes must interleave and the running count can never go
-- negative. A total that does is either a double-free in the application or a
-- close recorded for something we saw opened under a different handle.
SELECT 'more closes than opens for one subject (opener ' || c.opener || ')',
       c.rank, c.subject, c.n,
       (SELECT n FROM open_n o WHERE o.rank = c.rank AND o.subject = c.subject)
FROM closes c
WHERE c.n > COALESCE((SELECT n FROM open_n o WHERE o.rank = c.rank AND o.subject = c.subject), 0)

UNION ALL

-- The other direction, and the one this file could not ask until closes were
-- complete: an entity we saw opened and never saw closed. An unmatched CLOSE
-- names a missing creation door; an unmatched OPEN names a missing DESTRUCTION
-- door — ibv_destroy_qp has a dozen vendor-specific cousins and nothing here
-- would notice if an application used one.
--
-- Measured before it was asserted, on job 44396323 across four workloads:
-- opens and closes are exactly equal for every entity type that appears —
-- pingpong 2/2 regions and 2/2 QPs, send_bw the same, NCCL 1518/1518 and
-- 816/816, NVSHMEM coexist 716/716 and 432/432, multinode 1468/1468 and
-- 928/928. Not approximately; exactly, on every rank.
--
-- WHEN THIS FIRES, it has two readings and they are not the same bug:
--   * a destruction door we do not hook — the interesting one, and the mirror
--     of the six-hypothesis hunt that produced this file;
--   * an application that genuinely exits without cleaning up, which is legal
--     and not our problem. If that is what it is, scope the assertion to the
--     cases where cleanup is part of the workload rather than deleting it.
-- A third reading exists and should be ruled out first: a close that happened
-- after the last flush. That would make this flaky rather than red, and the
-- 4/4 exactness above is the evidence it does not happen today.
SELECT 'entity opened and never closed (opener ' || o.opener || ')',
       o.rank, o.subject, 0 AS closes, count(*) AS opens
FROM opens o
WHERE NOT EXISTS (SELECT 1 FROM closes c WHERE c.rank = o.rank AND c.subject = o.subject)
  -- ib_device (120) is exempt from THIS clause only, and the exemption is the
  -- second reading above rather than a workaround for it. A device context is
  -- the process's handle to the HCA: its natural lifetime is the process, and
  -- exiting without calling ibv_close_device on every one is legal, harmless,
  -- and what real MPI stacks do. Measured before exempting, on job 44399916:
  -- cases 02 and 07 each open 73 contexts per rank and close exactly 67, on
  -- every one of 8 ranks in both. That determinism is the evidence — a flush
  -- race or a missing destruction door would vary by rank, and this does not.
  --
  -- The other two clauses still cover ib_device in full, so a close naming a
  -- context nothing opened, or a context closed twice, is still caught. Only
  -- "never closed" is dropped, because for this entity it is not a defect.
  AND o.opener != 120
  -- mem_object (520) is exempt from THIS clause for a different reason than
  -- ib_device, and it is a gap rather than a property: gpumem hooks
  -- nvshmem_free but NOT cudaFree, and the NVSHMEM symmetric heap region is
  -- mapped for the life of the process and never freed at all. So an
  -- unclosed mem_object today means "we do not hook that destruction door
  -- yet", which is exactly what this clause is for — and firing it on every
  -- cudaMalloc in every capture would drown the signal rather than deliver it.
  --
  -- The exemption is therefore temporary and should be deleted the moment
  -- gpumem hooks cudaFree. The other two clauses still cover 520 in full: a
  -- close naming an object nothing recorded, or one closed twice, is caught.
  AND o.opener != 520
GROUP BY o.rank, o.subject, o.opener

ORDER BY 1, 2, 3;
