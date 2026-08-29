-- requires: ucx_rma ucx_rkey params.rma_iters
-- THE HEADLINE. Every one-sided row names a remote key, and that key must now
-- resolve to a row that recorded it being created.
--
-- Before `ucx_rkey` (626) existed, `ucx_rma.rkey` and `ucx_atomic.rkey` interned
-- a `ucp_rkey_h` that NOTHING opened — an entity referenced by events that
-- nothing creates, which is the exact defect `ib_hooks.hpp` documents chasing
-- for a week in the verbs provider, and which cost case 02 two of its eight
-- ranks when it was `ibv_dereg_mr`. Every one-sided capture this project has
-- ever taken carried that hole.
--
-- WHAT AN UNRESOLVED KEY WOULD MEAN, and why it is worth a whole assertion: the
-- RMA row would say a transfer happened and be unable to say which peer region
-- it targeted. `remote_addr` alone is a bare virtual address with no owner; it
-- is the rkey that ties it to an endpoint, and the endpoint to a peer through
-- ucx_ep_connection.
--
-- Scoped to keys ACTUALLY NAMED by a recorded operation rather than asserting a
-- count, because how many distinct keys MPI unpacks for one window is the MPI
-- implementation's business and not a fact this project should pin.
SELECT 'an RMA row names an rkey that no ucx_rkey row created' AS problem,
       r.rkey                                                 AS orphan_rkey,
       count(*)                                               AS rma_rows
FROM ucx_rma r
WHERE (SELECT rma_iters FROM params) > 0
  AND r.rkey <> 0
  AND NOT EXISTS (SELECT 1 FROM ucx_rkey k
                  WHERE k.rank = r.rank AND k.rkey = r.rkey)
GROUP BY r.rkey;
