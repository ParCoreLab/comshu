-- requires: ib_mem_region mem_object
-- Every registered memory region must have the memory object that describes it.
--
-- ibverbs emits both from one callback — the region (what the NIC was told) and
-- the object (what the buffer IS, i.e. which device owns it) — so this is
-- referential integrity on a single hook, not a claim about the workload. It is
-- worth running everywhere anyway, because it is also the POSITIVE CONTROL for
-- device attribution: an assertion that only asked "are any regions on a
-- device?" would read as a pass on a capture where the emitter never ran at
-- all. Here, a missing object is a named offending row.
--
-- Why the join is (rank, base, size) and not base alone: the same base is
-- registered more than once in real captures (NVSHMEM registers its heap and
-- then sub-ranges of it), and an address is only unique WITHIN a rank — the
-- same lesson comm_edges learned from qp_num, which collides across ranks and
-- resolved every colliding peer to whichever rank was scanned last.
--
-- Names no rank, no address and no count.
SELECT 'registered region with no mem_object' AS problem, r.rank, r.base, r.size, r.lkey
FROM ib_mem_region r
WHERE NOT EXISTS (
  SELECT 1 FROM mem_object m
  WHERE m.rank = r.rank AND m.base = r.base AND m.size = r.size
)
ORDER BY r.rank, r.base;
