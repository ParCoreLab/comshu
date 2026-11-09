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
-- 120 ib_device, 200 mlx5_umem, 202 mlx5_uar, 520 mem_object,
-- 600 ucx_context, 602 ucx_worker, 604 ucx_endpoint, 614 ucx_mem_map,
-- 626 ucx_rkey, 800 uct_iface, 802 uct_ep, 900 nccl_comm, 908 nccl_window.
--
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
  -- The four UCX entities that emit a close (ucx_provider.cpp closes 600, 602,
  -- 604 and 614; the rest are events or have no destruction door). They were
  -- missing from this CTE for the whole life of the UCX provider, which is the
  -- exact failure the note above predicts: case 19 reported 24 spurious "close
  -- names an entity no open record created" rows while its opens and closes
  -- matched EXACTLY — contexts {1,3,5,14} and workers {2,6,15} opened and closed
  -- on both ranks. The signal was noise, and it was hiding a real defect
  -- underneath (see the closes-outnumber-opens clause below).
  UNION ALL SELECT rank, context,  600 FROM ucx_context
  UNION ALL SELECT rank, worker,   602 FROM ucx_worker
  UNION ALL SELECT rank, endpoint, 604 FROM ucx_endpoint
  UNION ALL SELECT rank, memh,     614 FROM ucx_mem_map
  -- 626 ucx_rkey, and this list has now been left behind FOUR times: by the
  -- receive-path entities, by NCCL, by ucx_deep, and by this. Job 45177565 is
  -- the fourth — eight rkey closes, every one reported as naming an entity no
  -- open record created, because the record existed and this CTE did not know
  -- it. The failure is loud and harmless, which is exactly why it keeps
  -- recurring: nothing forces the two to be edited together. Any provider
  -- adding an Entity that emits EntityClose must add its line HERE, in the same
  -- change, or every close it writes reports as an orphan.
  --
  -- AND THEN IT FIRED FOR REAL, which is what this file is for. With 626 in
  -- this CTE, case 21 (job 45198862) still reports 39 and 38 unmatched closes
  -- per rank against 4 matched ones — and the door is
  -- `ucp_ep_rkey_unpack_internal`, which UCX's rendezvous protocol calls to
  -- create a key and then destroys through the PUBLIC ucp_rkey_destroy that we
  -- do interpose. Unmatched destroys come to ~2 per rendezvous on both ranks;
  -- case 22, which does MPI one-sided and no large rendezvous, has zero.
  --
  -- The symbol is exported, so it is hookable — but its signature gained a
  -- parameter BEFORE the out-parameter after v1.18.0, which is the version MN5
  -- runs, so a hook written against upstream would read the wrong register and
  -- intern a fabricated handle. It needs a runtime version gate. Left unhooked
  -- and left RED deliberately, per the paragraph above:
  -- plans/PLAN-20260830-1130-rendezvous-rkeys-have-no-creation-door-done.md
  UNION ALL SELECT rank, rkey,     626 FROM ucx_rkey
  -- The two NCCL entities that emit a close (nccl_provider.cpp closes 900 at
  -- ncclCommDestroy/ncclCommAbort and 908 at ncclCommWindowDeregister). Added
  -- WITH the provider, in the same change, because the paragraph above says
  -- what happens otherwise and the UCX case proved it: an entity type that
  -- emits EntityClose rows while absent from this CTE turns every one of its
  -- closes into a spurious "names an entity no open record created", and this
  -- assertion becomes noise on exactly the capture that needed it.
  UNION ALL SELECT rank, comm,   900 FROM nccl_comm
  UNION ALL SELECT rank, window, 908 FROM nccl_window
  -- ucx_deep's two entities, and this is the THIRD time the omission described
  -- above has actually happened — after the ib family and after UCX's four.
  -- ucx_deep landed on 2026-08-27 emitting EntityClose for both and adding
  -- neither line here, so case 21's first run reported 296 iface closes and 168
  -- endpoint closes as "names an entity no open record created" while opens and
  -- closes matched EXACTLY: 148 ifaces and 84 endpoints opened and closed on
  -- each of two ranks, zero unmatched on all three clauses once these two lines
  -- exist (job 45114566).
  --
  -- Three times is a pattern, not an accident, and the pattern is that this list
  -- is maintained by memory. It belongs beside the provider checklist in
  -- README.md's "Adding a provider" — a provider that closes an entity and does
  -- not appear here does not merely go unchecked, it turns this assertion into
  -- noise for every other provider in the same capture.
  UNION ALL SELECT rank, iface,  800 FROM uct_iface
  UNION ALL SELECT rank, ep,     802 FROM uct_ep
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
--
-- ONE INSTANCE OF THE SECOND READING IS KNOWN, and it is recorded here so the
-- next person to meet it at scale does not spend three jobs on it again.
-- Above 4 nodes, HPC-X's hcoll multicast component (hmca_mcast_vmc.so)
-- registers three regions per NODE LEADER and deregisters one, leaving exactly
-- 2 unclosed ib_mem_region on every rank with local rank 0 — 40 bytes and
-- 8388608 bytes, origin=reg_mr. A library exiting without freeing its own
-- buffers: legal, harmless, not ours. It is NOT exempted here, because a
-- global exemption for opener 100 would have hidden the two real defects this
-- file was written for (the CQ third door, and the 474 unmatched closes). It
-- is marked expect_fail on the one capture that crosses the threshold —
-- tests/hpc/corpus/scale_hubspoke_ib — and nowhere else. Evidence, and the
-- reason it is a leak rather than a door we miss:
-- plans/PLAN-20260812-0250-hcoll-mcast-leaks-two-mrs.
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
