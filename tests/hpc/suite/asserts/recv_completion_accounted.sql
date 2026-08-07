-- requires: ib_post_recv ib_poll_cq
-- Every recv completion joins to a posted receive, and no posted receive
-- completes twice.
--
-- This is the receive-side mirror of entity_close_accounted.sql, and it exists
-- for the same reason: until ib_post_recv landed, the receive half of every
-- transfer was INFERRED FROM ITS COMPLETION AND NEVER OBSERVED. pcomm hooked
-- ibv_post_send and ibv_poll_cq and no receive door at all, so a recv
-- completion's wr_id joined to nothing while the identical field on a send
-- completion joined to ib_post_send. A receive that was never posted and one
-- that completed normally produced exactly the same evidence: nothing, then a
-- completion, or nothing, then no completion.
--
-- WHY THIS IS AN ASSERTION AND NOT A GOLDEN. It names no rank, no qp_num, no
-- wr_id and no count. It does not reference params either, because it does not
-- need to: it is an invariant of the verbs API itself — the hardware cannot
-- complete a receive that was not posted — so it holds for pingpong, perftest,
-- NCCL and NVSHMEM alike, at whatever scale they run. A golden pinning "20 recv
-- completions" can only ever fail when the number changes; this fails when the
-- capture stops being internally coherent, which is the thing worth knowing.
--
-- THE JOIN KEY is (rank, qp_num, wr_id). All three are load-bearing:
--   * rank, because handles and wr_ids are per-process and a bare wr_id would
--     match another rank's receive — the discipline entity_close_accounted
--     learned and comm_edges learned the hard way from qp_num;
--   * qp_num, because wr_id is chosen by the application and is routinely a
--     small integer or a buffer index, so it repeats across queue pairs by
--     construction;
--   * wr_id, which is the only thing the completion carries back.
--
-- SRQ RECEIVES JOIN ON (rank, wr_id) AND NOT ON qp_num, and getting this wrong
-- is what the first version of this file did. An SRQ receive is not bound to a
-- queue pair when it is posted — the hardware picks the QP when a message
-- lands — so ib_post_recv rows of origin 'srq' carry qp_num = 0 while their
-- completions carry a real one. Scoping the *posts* to origin 'qp' without
-- scoping the *completions* to match therefore reported every SRQ-fed
-- completion as unposted.
--
-- That was measured, not reasoned: case 07 (NVSHMEM multinode) posts 131,728
-- SRQ receives against 15,360 QP receives and produced 656 false violations,
-- while case 02 passed only because NCCL creates 120 SRQs and never posts to
-- any of them. A capture with no SRQ traffic cannot tell you whether your SRQ
-- handling is right.
--
-- So a completion is accounted for if EITHER key matches, and the claim stays
-- as strong as it was: a completion matching neither still means a posting door
-- nobody hooked. Dropping qp_num for the SRQ arm loses precision — two SRQ
-- receives sharing a wr_id are indistinguishable — which is why the
-- double-completion check below stays scoped to the exact QP key.
--
-- TWO DIRECTIONS, NOT THREE, and the missing one is deliberate.
-- entity_close_accounted also asserts "opened and never closed", because an
-- entity that is never destroyed points at a destruction door we do not hook.
-- The analogous claim here — "posted and never completed" — is NOT a defect and
-- must never be asserted: posting receives ahead of need is the whole point of
-- the receive path, so a two-sided workload ends every run with unconsumed
-- buffers on the queue by design. NCCL's and NVSHMEM's proxy threads do it
-- continuously. Asserting it would fail on correct captures of correct
-- programs, which is how an assertion gets deleted instead of fixed.
WITH recv_cqe AS (
  SELECT CAST(rank AS INTEGER) AS rank, CAST(qp_num AS INTEGER) AS qp_num,
         wr_id, count(*) AS n
  FROM ib_poll_cq
  WHERE opcode IN ('recv', 'recv_rdma_imm') AND status = 'success'
  GROUP BY 1, 2, 3
),
-- Receives posted to a QP's own receive queue: keyed exactly.
posted AS (
  SELECT CAST(rank AS INTEGER) AS rank, CAST(qp_num AS INTEGER) AS qp_num,
         wr_id, count(*) AS n
  FROM ib_post_recv
  WHERE origin = 'qp'
  GROUP BY 1, 2, 3
),
-- Receives posted to a shared receive queue: no qp_num exists to key on.
posted_srq AS (
  SELECT DISTINCT CAST(rank AS INTEGER) AS rank, wr_id
  FROM ib_post_recv
  WHERE origin = 'srq'
)

-- 1. A completion for a receive nobody posted.
--
-- Read this exactly as entity_close_accounted's unmatched close is read: it is
-- evidence of a POSTING DOOR WE DO NOT HOOK, not of a broken completion path.
-- ibv_post_recv is one door and ibv_post_srq_recv is the other; if a third
-- exists — a provider-specific entry point, or a verbs_context slot of the kind
-- that hid ibv_create_qp_ex for a week — this is the row that says so. The
-- temptation when it fires will be to filter it. Don't. Find the door.
SELECT 'recv completion with no posted receive (rank, qp, wr_id)' AS problem,
       c.rank, c.qp_num, c.wr_id, c.n AS completions, 0 AS posts
FROM recv_cqe c
WHERE NOT EXISTS (SELECT 1 FROM posted p
                  WHERE p.rank = c.rank AND p.qp_num = c.qp_num AND p.wr_id = c.wr_id)
  AND NOT EXISTS (SELECT 1 FROM posted_srq s
                  WHERE s.rank = c.rank AND s.wr_id = c.wr_id)

UNION ALL

-- 2. More completions than posts for one (rank, qp, wr_id).
--
-- A wr_id is reusable — applications recycle them as buffer indices — but only
-- after the previous receive on it has completed, so posts and completions must
-- interleave and the running count can never go negative. A total that does
-- means either the hardware completed a receive twice (it does not) or we
-- recorded a completion for a post we missed, which is case 1 hiding behind a
-- wr_id that happens to be reused.
SELECT 'more recv completions than posted receives for one wr_id',
       c.rank, c.qp_num, c.wr_id, c.n,
       (SELECT n FROM posted p
        WHERE p.rank = c.rank AND p.qp_num = c.qp_num AND p.wr_id = c.wr_id)
FROM recv_cqe c
WHERE c.n > COALESCE((SELECT n FROM posted p
                      WHERE p.rank = c.rank AND p.qp_num = c.qp_num AND p.wr_id = c.wr_id), 0)
  -- An SRQ-fed completion has no QP-keyed post by construction, so without this
  -- it would trip the count test as surely as it tripped the existence test.
  AND NOT EXISTS (SELECT 1 FROM posted_srq s
                  WHERE s.rank = c.rank AND s.wr_id = c.wr_id)

ORDER BY 1, 2, 3, 4;
