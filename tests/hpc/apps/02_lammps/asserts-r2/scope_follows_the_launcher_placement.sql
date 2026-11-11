-- requires: comm_edges params.placement params.ranks params.ranks_per_node params.nodes params.heavy_share_pct
-- THE HEADLINE OF THIS ROUND. comm_edges.scope must say `network` for exactly
-- the edges that mpirun put on two different nodes, and `intra-node` for the
-- rest -- and the two arms of this study differ in NOTHING ELSE.
--
-- THE ORACLE IS THE LAUNCHER, NOT THE CAPTURE. `mpirun --map-by ppr:4:node`
-- fills a node before moving on, so node(r) = r / ranks_per_node. `mpirun
-- --map-by node` deals round robin, so node(r) = r % nodes. That is Open MPI's
-- documented mapping policy and the job script confirms it independently by
-- printing `hostname` from every rank of the BARE run, before any capture
-- exists. `placement` selects which rule applies; nothing here reads
-- rank_identity, node_identity or any other comshu table.
--
-- WHY THE PAIR OF ARMS IS THE EXPERIMENT. The decomposition, the peer set, the
-- message sizes and the byte totals are byte-identical between them
-- (PREDICTION-r2.md §1). Only the placement moves. So this assertion demands
-- that scope report the exact COMPLEMENT in the second arm: the 32 edges that
-- were `network` become `intra-node` and vice versa. A scope column that is
-- derived from anything other than node membership -- from the transport name,
-- from a rank-distance heuristic, from the order the ranks were seen -- passes
-- one arm and fails the other, and a column that is simply constant fails both.
--
-- WHAT IT CATCHES:
--   * clause 1: an edge labelled `network` whose two ranks share a node, or
--     `intra-node` whose two ranks do not. This is the substantive claim.
--   * clause 2: `unknown`, or NULL, on a heavy edge. The brief says `unknown`
--     means a peer rank or a node identity could not be resolved and is never
--     folded into `network` -- so it is not an error in itself, but on an edge
--     carrying a fifth of a rank's traffic it means the topology view has a hole
--     and every share computed from it is over an incomplete denominator.
--   * clause 3: a value outside the three the contract declares.
--
-- Restricted to HEAVY edges deliberately. Open MPI's collective topology and
-- UCX's wireup produce a residue of tiny edges whose scope is just as
-- checkable, but whose peer set is Open MPI's business and not LAMMPS's -- and
-- an assertion that cannot say what the right answer is has no business
-- demanding one. heavy_peers_are_the_grid_neighbours.sql pins the same edge set
-- from the other side.
WITH tot AS (
  SELECT src_rank, sum(CAST(bytes AS BIGINT)) AS total FROM comm_edges GROUP BY src_rank
), heavy AS (
  SELECT e.src_rank, e.dst_rank, e.scope, sum(CAST(e.bytes AS BIGINT)) AS bytes
  FROM comm_edges e JOIN tot t ON t.src_rank = e.src_rank
  GROUP BY e.src_rank, e.dst_rank, e.scope
  HAVING sum(CAST(e.bytes AS BIGINT)) * 100 >= max(t.total) * (SELECT heavy_share_pct FROM params)
), placed AS (
  SELECT h.*,
         CASE WHEN (SELECT placement FROM params) = 0
              THEN h.src_rank // (SELECT ranks_per_node FROM params)
              ELSE h.src_rank % (SELECT nodes FROM params) END AS src_node,
         CASE WHEN (SELECT placement FROM params) = 0
              THEN h.dst_rank // (SELECT ranks_per_node FROM params)
              ELSE h.dst_rank % (SELECT nodes FROM params) END AS dst_node
  FROM heavy h
)
SELECT 'scope disagrees with where mpirun put the two ranks' AS problem,
       src_rank, dst_rank, src_node, dst_node, scope,
       CASE WHEN src_node = dst_node THEN 'intra-node' ELSE 'network' END AS expected, bytes
FROM placed
WHERE scope IS NOT NULL AND scope IN ('intra-node','network')
  AND scope <> CASE WHEN src_node = dst_node THEN 'intra-node' ELSE 'network' END

UNION ALL

SELECT 'a heavy edge has unresolved scope',
       src_rank, dst_rank, src_node, dst_node, COALESCE(scope,'<null>'),
       CASE WHEN src_node = dst_node THEN 'intra-node' ELSE 'network' END, bytes
FROM placed
WHERE scope IS NULL OR scope = 'unknown'

UNION ALL

SELECT 'scope took a value outside the three the contract declares',
       src_rank, dst_rank, src_node, dst_node, COALESCE(scope,'<null>'), '-', bytes
FROM placed
WHERE scope IS NOT NULL AND scope NOT IN ('intra-node','network','unknown')

UNION ALL

-- ANTI-VACUITY. `heavy` reaches comm_edges through an inner join against the
-- per-rank total, so an empty comm_edges empties `placed` and all three clauses
-- above report nothing while asserting nothing. Both other assertions in this
-- directory that had this shape have now been caught doing exactly that once
-- each; this one says so up front.
SELECT 'comm_edges is empty, so this assertion checked nothing',
       -1, -1, -1, -1, '-', '-', 0
WHERE (SELECT count(*) FROM comm_edges) = 0;
