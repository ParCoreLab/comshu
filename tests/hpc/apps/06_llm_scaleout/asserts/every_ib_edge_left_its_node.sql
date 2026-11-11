-- requires: comm_edges params.ranks params.ranks_per_node params.nodes
-- THE HEADLINE. On a capture whose only wire provider is ibverbs, every edge in
-- comm_edges must have crossed the node boundary -- checked twice, once against
-- comshu's own `scope` column and once against the launcher's placement.
--
-- WHY IT MUST BE TRUE, AND THE ARGUMENT IS NCCL'S, NOT COMSHU'S. NCCL moves data
-- between GPUs inside a node over NVLink peer-to-peer -- a device-side store,
-- which posts no verbs work request and reaches no ibverbs door -- and between
-- nodes over its `net` transport, which is libibverbs. Neither half is a claim
-- about comshu; NCCL prints both, one line per connection, under
-- NCCL_DEBUG=INFO: `via P2P/direct pointer` against `via NET/IB/<hca>`. The bare
-- arm of job.sbatch collects exactly that, so the oracle exists on paper before
-- this query is run.
--
-- SO A GRADIENT ALL-REDUCE AT 16 GPUs IS HIERARCHICAL BY CONSTRUCTION, and this
-- is the assertion that says the derived layer sees the hierarchy: the
-- intra-node half is invisible to this capture and the inter-node half is all of
-- it.
--
-- CLAUSE 1 IS THE ONE THAT COSTS SOMETHING. `comm_edges.scope` is derived from
-- NODE MEMBERSHIP, not from the transport, so for it to read `network` the link
-- must have resolved a chain nobody has ever exercised on NCCL traffic: queue
-- pair -> LID/QPN -> peer rank -> which node that rank is on. Round 1 of
-- 04_llmc read `ib_post_send` raw and never ran `comshu link` on a multi-node
-- capture at all, so this chain has no measurement behind it.
--
-- CLAUSE 2 IS THE SAME CLAIM WITHOUT CONSULTING comshu'S ANSWER. `--map-by
-- ppr:4:node` gives node(r) = r / ranks_per_node, confirmed independently by
-- printing hostname from every rank of the bare run. If clause 1 passes and
-- clause 2 fails, `scope` is self-consistent and wrong -- which is a defect a
-- single-column check could not distinguish from success.
--
-- CLAUSE 3 IS THE ANTI-VACUITY GUARD AND IT IS NOT OPTIONAL. Both clauses above
-- are "no offending rows" over comm_edges, so an EMPTY comm_edges passes them
-- both while asserting nothing -- which is exactly the failure round 1 of
-- 02_lammps shipped three times (its NOTES.md #7). An empty comm_edges on a
-- capture holding tens of thousands of ib_post_send rows is not a pass, it is
-- the study's other possible headline, and clause 3 makes it show up red.
SELECT 'an edge is labelled intra-node on a capture whose only wire lane is ibverbs' AS problem,
       src_rank, dst_rank, scope, sends, bytes
FROM comm_edges
WHERE scope IS NULL OR scope <> 'network'

UNION ALL

SELECT 'an edge joins two ranks that mpirun put on the same node',
       src_rank, dst_rank, scope, sends, bytes
FROM comm_edges
WHERE src_rank // (SELECT ranks_per_node FROM params)
    = dst_rank // (SELECT ranks_per_node FROM params)

UNION ALL

SELECT 'comm_edges is empty, so this assertion checked nothing',
       0, 0, '-', 0, 0
WHERE (SELECT count(*) FROM comm_edges) = 0

UNION ALL

-- A rank number outside 0..ranks-1 would mean the peer resolution produced an
-- identifier that is not a rank at all. Cheap, and it is the shape of failure a
-- LID/QPN collision across four nodes would take.
SELECT 'an edge names a rank outside the communicator',
       src_rank, dst_rank, scope, sends, bytes
FROM comm_edges
WHERE src_rank < 0 OR dst_rank < 0
   OR src_rank >= (SELECT ranks FROM params)
   OR dst_rank >= (SELECT ranks FROM params);
