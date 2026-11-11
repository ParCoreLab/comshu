-- requires: mem_owner
-- An address has one owner, and its owner has one device.
--
-- mem_owner is comshu-read's containment join: every referenced address resolved
-- against that RANK's memory objects. Rank scope is the whole discipline here —
-- comm_edges learned it from qp_num, which is HCA-local and made a qp_num-keyed
-- map resolve every colliding peer to whichever rank was scanned last. Virtual
-- addresses are worse: every rank's heap starts in the same place.
--
-- Two failures, deliberately labelled apart because they mean different things:
--
--   ambiguous_refs > 0 — the address landed inside two objects whose ranges
--   PARTIALLY OVERLAP, i.e. neither contains the other. No rule picks a winner
--   without inventing one. This is the predicted failure of the
--   shared-vocabulary design: any provider may hook CUDA itself, so two of them
--   can observe one allocation, and hooking both ibv_reg_mr and
--   ibv_reg_mr_iova2 already did this shape to case 01 — same mr, same base,
--   same lkey, every region twice.
--
--   NESTED ranges are explicitly NOT this. A containment chain resolves to the
--   innermost object, the way the innermost enclosing scope names a variable,
--   and is counted in nested_refs instead. This assertion originally failed on
--   those too and went red on a correct capture (MN5 44348796): NCCL registers
--   co-terminal sub-ranges of one buffer — 0x2374f70+4448, 0x23753c0+3344,
--   0x2375810+2240, 0x2375c60+1136, all ending at 0x23760D0. Ordinary allocator
--   behaviour. The fix was in the resolution rule, not in weakening the check:
--   partial overlap still fails, and nothing was made permissible that was not
--   already correct.
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
