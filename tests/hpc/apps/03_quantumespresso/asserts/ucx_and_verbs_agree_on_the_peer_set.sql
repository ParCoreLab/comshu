-- requires: ucx_tag_send ucx_endpoint comm_edges params.ranks
-- The two layers in one capture must agree on HOW MANY peers each rank has.
--
-- WHAT THIS DELIBERATELY DOES NOT CLAIM. It does not claim that a ucp_tag_send
-- corresponds to an ibv_post_send. That is PLAN-20260815-1540 stage 3, and it is
-- blocked for a measured reason: MN5's UCX posts its sends through the mlx5
-- doorbell, below every interposable verbs symbol (job 44635003, reproduced as
-- run 3 of this job). A message-level correspondence is not merely unproven
-- here, it is unobservable, and an assertion that pretended otherwise would go
-- red on a working tool.
--
-- WHAT IT DOES CLAIM, and why this one is answerable. Endpoint SETUP is not on
-- the fast path. UCX creates exactly one ucp_ep per peer it talks to, and builds
-- that endpoint's queue pair through the ordinary verbs doors — libuct_ib.so's
-- undefined symbols include ibv_create_qp, ibv_modify_qp and ibv_query_port
-- (login-node sweep, 2026-08-26). So the capture holds two independently derived
-- counts of the same quantity, and they must bracket each other:
--
--   lower bound : a rank's UCP endpoint count is at least the number of distinct
--                 peer ranks the verbs layer resolved for it — you cannot carry
--                 traffic to a peer you never dialled.
--   upper bound : it is at most params.ranks — you cannot dial more peers than
--                 exist, self included.
--
-- A violation of the lower bound means one of the two attributions is wrong, and
-- the row prints both numbers so the reader can see which side is short. If they
-- disagree I believe the VERBS side, for the reason case 01's topology golden
-- taught this project: the verbs peer is derived from the (src_lid, qp_num) pair
-- the fabric itself assigned and the RTR transition recorded, whereas the UCX
-- endpoint count is a count of local handles with no cross-rank check available
-- at all (see ucx_peer_matrix_is_recoverable.sql — the door that would provide
-- one is not hooked). A number with an independent cross-rank witness beats a
-- number without one.
--
-- Names no rank, no LID, no qp_num and no endpoint handle: every quantity is a
-- per-rank count, and the bounds come from params.
WITH ucx_deg AS (
    SELECT CAST(rank AS INTEGER) AS rank, count(DISTINCT endpoint) AS eps
    FROM ucx_tag_send GROUP BY 1
),
ib_deg AS (
    SELECT CAST(src_rank AS INTEGER) AS rank, count(DISTINCT CAST(dst_rank AS INTEGER)) AS peers
    FROM comm_edges
    WHERE CAST(dst_rank AS INTEGER) >= 0 AND CAST(src_rank AS INTEGER) <> CAST(dst_rank AS INTEGER)
      AND CAST(sends AS INTEGER) > 0
    GROUP BY 1
)
SELECT 'rank carried verbs traffic to more peers than it dialled UCP endpoints' AS problem,
       i.rank, u.eps AS ucp_endpoints, i.peers AS verbs_peers
FROM ib_deg i LEFT JOIN ucx_deg u ON u.rank = i.rank
WHERE coalesce(u.eps, 0) < i.peers
UNION ALL
SELECT 'rank dialled more UCP endpoints than there are ranks',
       u.rank, u.eps, (SELECT ranks FROM params)
FROM ucx_deg u
WHERE u.eps > (SELECT ranks FROM params)
ORDER BY 2;
