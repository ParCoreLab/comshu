-- requires: comm_edges rank_identity
-- comm_edges.scope must agree with where the two ranks actually live.
--
-- `scope` was added 2026-09-01 because comm_edges looked like the fabric matrix
-- and was not one: three round-2 applications put intra-node UCX traffic into
-- it indistinguishable from traffic that crossed the wire (OSU 2 operations,
-- GROMACS 83,892, Quantum ESPRESSO 1,435,037). The column is derived from node
-- membership rather than from transport, so it is checkable against the same
-- node identity every other table uses — which is what this file does.
--
-- THREE CLAUSES, each catching a different way the derivation could rot.
--
-- Names no rank, no node and no count: every clause is a comparison between two
-- derived tables, so it holds on one node and on sixteen.
--
-- WHY NOT A TAUTOLOGY. It would be, if it re-derived scope from _node the same
-- way comm_edges does. It does not: it goes through `rank_identity.node_ix`,
-- which is the published per-rank node ordinal a UI or a user reads. So this
-- asserts the two agree, and a change to either one that desynchronises them
-- lands here rather than in someone's headline number.

-- 1. An edge between two ranks the identity table places on ONE node must not
--    be called 'network'. This is the defect the column was added for, and the
--    only clause that would have fired before it existed.
SELECT 'comm_edges calls an edge network, but both ranks are on one node' AS problem,
       e.transport,
       e.scope,
       count(*) AS edges
FROM comm_edges e
JOIN rank_identity s ON s.rank = e.src_rank
JOIN rank_identity d ON d.rank = e.dst_rank
WHERE e.dst_rank >= 0
  AND s.node_ix = d.node_ix
  AND e.scope <> 'intra-node'
GROUP BY e.transport, e.scope

UNION ALL

-- 2. The mirror. Two ranks on different nodes cannot have talked without
--    leaving one, whatever transport carried it.
SELECT 'comm_edges calls an edge intra-node, but the ranks are on different nodes',
       e.transport,
       e.scope,
       count(*)
FROM comm_edges e
JOIN rank_identity s ON s.rank = e.src_rank
JOIN rank_identity d ON d.rank = e.dst_rank
WHERE e.dst_rank >= 0
  AND s.node_ix <> d.node_ix
  AND e.scope <> 'network'
GROUP BY e.transport, e.scope

UNION ALL

-- 3. 'unknown' must mean something is genuinely unknown. An edge whose peer
--    resolved AND whose both ranks are placed has no excuse for it — that would
--    be the CASE falling through, which is how a third value quietly becomes a
--    dumping ground.
--
--    The converse is deliberately NOT asserted: an edge with dst_rank = -1, or
--    a rank absent from rank_identity, is *expected* to be 'unknown', and there
--    are captures where that is most of the table (a pure-NVLink run with no
--    node identity — see link_health.unresolved_nvlink).
SELECT 'comm_edges scope is unknown, though both ranks are placed and the peer resolved',
       e.transport,
       e.scope,
       count(*)
FROM comm_edges e
JOIN rank_identity s ON s.rank = e.src_rank
JOIN rank_identity d ON d.rank = e.dst_rank
WHERE e.dst_rank >= 0
  AND e.scope = 'unknown'
GROUP BY e.transport, e.scope

ORDER BY 1, 2, 3;
