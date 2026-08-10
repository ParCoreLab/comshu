-- requires: mem_object params.app_buffer_bytes
-- The buffers the APPLICATION allocated must be recorded, whichever providers
-- were loaded.
--
-- The size comes from the workload's own `app_buffer_bytes`, which is what
-- putget passes to nvshmem_malloc — a number the command line fixes, not one
-- read back off a capture. That is the difference between an assertion and a
-- golden, and it is why this file names no rank, no address and no count.
--
-- It gates on `app_buffer_bytes` rather than on `elems * elem_bytes`, and that
-- is not tidiness. Cases 02, 03 and 05 also declare elems and elem_bytes, but
-- they are NCCL and plain-CUDA workloads that never call nvshmem_malloc — NCCL
-- allocates its own buffers and `elems` is a collective's element count, not an
-- allocation. Deriving the size from elems would have fired on all three, and a
-- cross-cutting assertion that is wrong on a third of the board is noise
-- exactly when it is needed. A case opts in by declaring what its application
-- allocates.
--
-- WHAT IT IS FOR. Until the object registry moved into the core, this question
-- had a different answer depending on which provider ran. The same NVSHMEM
-- workload captured with mlx5dv alone held 20 mem_object rows of 8, 40 and
-- 327680 bytes — NVSHMEM's own control structures and work queues, because
-- NVSHMEM registers what *it* hands the NIC — and the two 4096-byte buffers the
-- program asked for were absent entirely. Captured with gpumem they were there.
-- Since NVBit costs ~435 s of module lifting (case 05), "what did this program
-- allocate" was gated behind the most expensive provider in the tool.
--
-- The transport's rows were never wrong, and this assertion does not say they
-- were: an MR or a umem records real memory at a real address. It records the
-- TRANSPORT's buffer. An application object and a transport artifact are
-- different things that share a record type, and counting mem_object rows makes
-- them look interchangeable — which is exactly how the gap went unnoticed.
--
-- WHEN THIS FIRES, the reading is narrow: nothing interposed the allocator on
-- this run. Either the engine was linked but not preloaded (the warning
-- pcomm_engine_interposer_live exists for), or the workload allocates through a
-- door the core does not hook yet. It is NOT a reason to relax the size check —
-- a capture that cannot name the buffer under test cannot answer the object
-- questions the UI asks of it.
--
-- Scoped per rank, like every other assertion here: handles and addresses are
-- per-process, and a buffer present on one rank says nothing about another.
--
-- CAST because sql_check.sh loads these CSVs into sqlite, where every column is
-- TEXT and "1000" sorts below "999".
SELECT 'rank recorded no mem_object of the application buffer size' AS problem,
       r.rank,
       (SELECT app_buffer_bytes FROM params) AS expected_size,
       (SELECT count(*) FROM mem_object m WHERE m.rank = r.rank) AS objects_on_rank
FROM (SELECT DISTINCT rank FROM mem_object) r
WHERE NOT EXISTS (
    SELECT 1
    FROM mem_object m
    WHERE m.rank = r.rank
      AND CAST(m.size AS INTEGER) = (SELECT app_buffer_bytes FROM params))
ORDER BY r.rank;
