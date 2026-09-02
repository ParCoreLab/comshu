-- requires: nccl_collective nccl_group params.ranks params.iters params.groups_per_iter
-- llm.c's group structure, which is what makes its collectives one device kernel
-- each, and the emptiness of nccl_p2p, which is why comm_edges has nothing to
-- build a collective lane from.
--
-- THE GROUP ORACLE. zero.cuh:530 and :551 bracket the whole backward-pass
-- reduction in one ncclGroupStart/End per layer plus one, so L + 1 = 7 groups
-- per optimizer step at ZeRO stage 0; at stage 1 a further
-- NUM_PARAMETER_TENSORS = 16 come from gpt2_update (train_gpt2.cu:1110-1118),
-- giving 23. nccl_group records a row per BOUNDARY, so the row count is twice
-- the number of groups. llm.c states the intent itself at :530 -- "NCCL group:
-- aggregate all pointers in a single NCCL GPU kernel" -- and pcomm.fbs type 906
-- states it from the other side: "N grouped sends are ONE launch, and nothing in
-- a send row says so". Two independent sources, neither of them a capture.
--
-- WHY IT IS HERE AND NOT ONLY IN 04_llmc. That study checked the group count
-- against the KERNEL count on one node. This one has no kernel provider in its
-- primary arm, and the reason to re-assert the group count at 16 ranks is
-- different: `groups_per_iter` is P-independent, so a change at sixteen ranks
-- would mean NCCL's group nesting behaves differently at scale, which is a fact
-- about NCCL 2.x that this project has no other way to notice.
--
-- THE P2P CLAUSE IS THE LOAD-BEARING ONE FOR THIS STUDY. pcomm.fbs type 904
-- says NcclP2p is "the only NCCL call that names a peer, and so the only one an
-- exact edge follows from". llm.c calls ncclSend/ncclRecv zero times. So every
-- rank-to-rank edge this study finds must have come from the VERBS lane and not
-- from the collective lane -- which is exactly what makes
-- every_ib_edge_left_its_node.sql a statement about the fabric rather than about
-- a topology pcomm inferred. If nccl_p2p is ever non-empty here, that assertion
-- changes meaning and this clause is how anyone would find out.
--
-- ON REQUIRING nccl_p2p: it is deliberately NOT in the `-- requires:` header.
-- 04_llmc's NOTES.md records the sharp edge -- `-- requires: nccl_p2p` reports
-- [skip] for an empty-but-present table, so an assertion that wants to CLAIM
-- emptiness must not require its own table. The runner's presence test makes a
-- genuinely absent table report as absent rather than as a parse error, and the
-- Parquet exporter omits empty raw tables entirely, so the count below is taken
-- through a scalar subquery that tolerates both.
SELECT 'a rank did not record the expected number of group boundaries' AS problem,
       rank, n, (SELECT 2 * groups_per_iter * iters FROM params)
FROM (SELECT rank, count(*) AS n FROM nccl_group GROUP BY rank) x
WHERE n <> (SELECT 2 * groups_per_iter * iters FROM params)

UNION ALL

SELECT 'not every rank recorded groups',
       (SELECT count(DISTINCT rank) FROM nccl_group), (SELECT ranks FROM params), 0
WHERE (SELECT count(DISTINCT rank) FROM nccl_group) <> (SELECT ranks FROM params)

UNION ALL

-- Two streams per rank and no more: llm.c has exactly nccl_stream and
-- main_stream. Counted PER RANK, never globally -- intern handles restart per
-- rank, so a global count(DISTINCT stream) reads 3 for a two-stream workload
-- and 04_llmc nearly filed that as a prediction failure.
SELECT 'a rank used a number of CUDA streams llm.c does not have',
       rank, count(DISTINCT stream), 2
FROM nccl_collective GROUP BY rank
HAVING count(DISTINCT stream) <> 2;
