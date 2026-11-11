-- requires: tag_flow transport_mix
-- If the hardware tag-matching doors fired, tag_flow must join the two layers.
-- If they did not, that is the fabric's doing and not comshu's, and the two must
-- be distinguishable.
--
-- WHAT tag_flow CLAIMS. It is advertised as the join between what the
-- application ASKED UCX for (the 600s) and what UCX put on the wire (the 800s),
-- keyed on the tag. That join can only exist where uct_send.tag is populated,
-- and case 21's tag_and_addr_are_door_scoped.sql states that tag is written by
-- the TagEager*/TagRndv* doors alone -- the ConnectX offload -- and is 0 on
-- every other door, because without offload UCP does its own matching and the
-- tag travels inside an active-message payload the UCT record does not parse.
--
-- WHAT THIS APPLICATION PREDICTS. MN5's HPC-X does not enable ConnectX tag
-- matching by default; case 21 has to set UCX_RC_MLX5_TM_ENABLE=y and pin
-- UCX_TM_THRESH to reach those doors at all. GROMACS sets no UCX variable and
-- neither does Open MPI. So in the two default arms of this job, tag_flow is
-- predicted to contain no matched flow whatsoever -- which, if it holds, means
-- the table advertised as the intent-to-wire join is structurally empty for the
-- most common MPI stack on this machine. That is a finding about scope, not a
-- bug, and it is worth a row.
--
-- THE tm ARM IS THE CONTROL. It sets the two TM variables and a threshold below
-- GROMACS's message sizes so the doors CAN fire. With that arm beside these two,
-- the three outcomes separate cleanly:
--   * tag doors absent in all arms  -> offload never engages on this fabric;
--   * tag doors present, no matched flow -> THE JOIN IS WRONG. A defect.
--   * tag doors present, flows matched -> the table works on a real application,
--     which nothing has yet shown.
-- Only the middle one is comshu's, and only this shape of query tells it apart.
--
-- BOTH REPRESENTATIONS OF `op` ARE NAMED, and that is load-bearing rather than
-- belt-and-braces. The CSV export decodes op to a name while the Parquet export
-- leaves the raw integer, and comshu.fbs declares no enum for it -- the comment
-- on UctSend points at providers/ucx_deep/ucx_deep_records.hpp, which a reader
-- of the schema does not have. An assertion naming only the strings returns no
-- row on Parquet and passes vacuously; naming only the integers does the same on
-- CSV. The integers below are the ones case 21's assertions pin.
SELECT 'hardware tag doors fired but not one UCP send joined a wire operation' AS problem,
       NULL AS rank, NULL AS tag, NULL AS ucp_sends, NULL AS uct_ops
WHERE EXISTS (SELECT 1 FROM transport_mix
              WHERE CAST(op AS VARCHAR) IN ('10','11','12','13',
                                            'tag_eager_short','tag_eager_bcopy',
                                            'tag_eager_zcopy','tag_rndv_zcopy'))
  AND NOT EXISTS (SELECT 1 FROM tag_flow WHERE status = 'tag_matched')

UNION ALL

-- A matched flow that used a single door, and whose byte counts agree at both
-- layers, is an eager flow: one UCP send is one UCT operation by construction.
-- Rendezvous is excluded without being named, because its payload is advertised
-- rather than sent and so uct_payload <> ucp_units there. Copied in shape from
-- case 21, because it is the right shape and this is the first application to
-- put a real message-size distribution through it.
SELECT 'a single-door eager flow disagrees on how many operations it took',
       rank, tag, ucp_sends, uct_ops
FROM tag_flow
WHERE status = 'tag_matched'
  AND CAST(doors AS INTEGER) = 1
  AND CAST(uct_payload AS INTEGER) = CAST(ucp_units AS INTEGER)
  AND CAST(ucp_sends AS INTEGER) <> CAST(uct_ops AS INTEGER)

UNION ALL

-- The tm arm's own claim, stated from the parameter rather than from anything
-- observed, so that a tm run which reaches no tag door is a visible result and
-- not a silent one.
SELECT 'the arm that asked for tag offload reached no hardware tag-matching door',
       NULL, NULL, (SELECT tm_enable FROM params), NULL
WHERE (SELECT tm_enable FROM params) = 1
  AND NOT EXISTS (SELECT 1 FROM transport_mix
                  WHERE CAST(op AS VARCHAR) IN ('10','11','12','13',
                                                'tag_eager_short','tag_eager_bcopy',
                                                'tag_eager_zcopy','tag_rndv_zcopy'));
