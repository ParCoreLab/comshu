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
--
-- TWO EXEMPTIONS, both named explicitly rather than folded into the clause
-- below, and both keyed on `origin` — which door registered the region — rather
-- than on base=0, which an ordinary broken registration also satisfies.
--
-- SECOND EXEMPTION: a region with no host address BY CONSTRUCTION.
-- origin='device_memory' is ibv_reg_dm_mr, where the memory lives on the NIC,
-- and origin='null_mr' is ibv_alloc_null_mr, which has no memory at all. For
-- these, base=0 is the truth and not a failure to resolve, so requiring a
-- mem_object would require inventing one. Found by MN5 job 44368870: 474 closes
-- per run named regions nothing opened, all addr=NULL, because both doors are
-- `static inline` over verbs_context function pointers and no symbol tool could
-- ever name them.
--
-- FIRST EXEMPTION, named explicitly rather than folded into the clause below: a
-- dmabuf-registered region (dmabuf_fd >= 0) whose address could not be
-- recovered. Such a region has no host address of its own — whether one exists
-- depends on the caller, and NCCL demonstrates both cases in one codebase,
-- passing the real address as `iova` on its net_ib path (reg.cc:35) and 0 on its
-- GDAKI path (gin_host_gdaki.cc:131). Emitting a MemObject at base 0 to satisfy
-- this assertion would put a fiction in the capture that range containment then
-- resolves real addresses against, so the region is recorded and the object is
-- not. The exemption is `base = 0x0 AND dmabuf_fd >= 0`, which is as narrow as
-- it can be: an ordinary registration at base 0 still fails, and a dmabuf region
-- WITH an address is still required to have its object.
SELECT 'registered region with no mem_object' AS problem, r.rank, r.base, r.size, r.lkey
FROM ib_mem_region r
WHERE NOT (r.base = '0x0' AND CAST(r.dmabuf_fd AS INTEGER) >= 0)
  AND r.origin NOT IN ('device_memory', 'null_mr')
  AND NOT EXISTS (
  SELECT 1 FROM mem_object m
  WHERE m.rank = r.rank AND m.base = r.base AND m.size = r.size
)
ORDER BY r.rank, r.base;
