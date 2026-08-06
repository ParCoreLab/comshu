-- requires: mem_owner
-- An address has one owner, and its owner has one device.
--
-- mem_owner is pcomm-read's containment join: every referenced address resolved
-- against that RANK's memory objects. Rank scope is the whole discipline here —
-- comm_edges learned it from qp_num, which is HCA-local and made a qp_num-keyed
-- map resolve every colliding peer to whichever rank was scanned last. Virtual
-- addresses are worse: every rank's heap starts in the same place.
--
-- Two failures, deliberately labelled apart because they mean different things:
--
--   ambiguous_refs > 0 — the address landed inside more than one object. This is
--   the predicted failure of the shared-vocabulary design: any provider may hook
--   CUDA itself, so two of them can observe one allocation, and hooking both
--   ibv_reg_mr and ibv_reg_mr_iova2 already did exactly this shape to case 01 —
--   same mr, same base, same lkey, every region twice. The reader merges
--   emitters that agree on (base, size); what reaches here is two objects with
--   DIFFERENT extents claiming one address, which no merge can reconcile.
--
--   devices > 1 — the emitters agree the buffer exists and disagree about which
--   GPU owns it. That is a finding, not a tie to break: the reader prints the
--   lowest ordinal so the table is readable, and this row says the value is not
--   to be trusted. Do not resolve it by teaching one emitter to defer to
--   another; a provider needs allocation facts for its own runtime decisions and
--   cannot wait on a second provider for them.
--
-- Names no rank, no address, no device and no count.
SELECT 'address resolves to more than one mem_object' AS problem,
       rank, source, base, size, device, devices, emitters, refs, ambiguous_refs
FROM mem_owner
WHERE CAST(ambiguous_refs AS INTEGER) > 0

UNION ALL

SELECT 'emitters disagree on the device owning this buffer',
       rank, source, base, size, device, devices, emitters, refs, ambiguous_refs
FROM mem_owner
WHERE CAST(devices AS INTEGER) > 1

ORDER BY 1, 2, 4;
