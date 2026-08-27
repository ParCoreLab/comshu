-- requires: tag_flow params.msg_zcopy_bytes
-- Every size the workload sends with hardware tag matching must appear as a
-- MATCHED flow, and the matched flows must agree on how many sends there were.
--
-- This is the one assertion in the suite that spans both provider ranges. The
-- 600s record what the application asked UCX for; the 800s record what UCX put
-- on the wire; `tag_flow` joins them on the tag, and it can only do that because
-- `uct_send.tag` was measured to carry the application's own tag rather than
-- something UCX invented (job 45115011).
--
-- TWO CLAIMS, and they fail for different reasons.
--
-- 1. There is at least one matched flow. If tag matching engaged at all — and
--    tag_offload_was_reached.sql asserts separately that it did — then some UCP
--    send must join a UCT operation. Zero matched flows with tag doors present
--    means the two layers disagree about the tag, which is the join being wrong
--    rather than the workload being quiet.
--
-- 2. A matched flow's two send counts agree. The workload sends `iters` messages
--    per size per rank through one door each; when the door is a tag door, one
--    UCP send is one UCT operation, so the counts must be equal. They are
--    ALLOWED to differ in general — a rendezvous flow is one UCP send and one
--    advertisement, and a fragmented one could be several — so this checks only
--    the eager doors, where the relationship is one to one by construction.
--
-- NAMES NO TAG AND NO SIZE. The tag values are Open MPI's encoding of the
-- workload's MPI tags and would be a golden if written down; the counts come
-- from `iters` in meta.json.
SELECT 'hardware tag matching engaged but no UCP send joined a wire operation' AS problem,
       NULL AS rank, NULL AS tag, NULL AS ucp_sends, NULL AS uct_ops
WHERE EXISTS (SELECT 1 FROM tag_flow WHERE status = 'wire_only' OR status = 'tag_matched')
  AND NOT EXISTS (SELECT 1 FROM tag_flow WHERE status = 'tag_matched')

UNION ALL

-- `doors = 1` restricts this to a flow that used a single door; `uct_payload =
-- ucp_units` restricts it to one where the bytes agree, which excludes the
-- rendezvous door (whose payload is advertised, not sent) without naming it.
-- What is left is the eager case, where one send is one operation.
SELECT 'a single-door eager flow disagrees on how many operations it took',
       rank, tag, ucp_sends, uct_ops
FROM tag_flow
WHERE status = 'tag_matched'
  AND CAST(doors AS INTEGER) = 1
  AND CAST(uct_payload AS INTEGER) = CAST(ucp_units AS INTEGER)
  AND CAST(ucp_sends AS INTEGER) <> CAST(uct_ops AS INTEGER);
