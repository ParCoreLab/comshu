-- requires: tag_flow uct_send ucx_tag_send
-- `tag_flow` has never been pointed at an application we did not write. This is
-- that test, and the two clauses fail for different reasons.
--
-- tag_flow is the ONLY thing in comshu that joins the 600s (what the application
-- asked UCX for) to the 800s (what UCX put on the wire). demos/figures/
-- 09-ucx-records.svg states the join outright: "The 600s and the 800s are joined
-- on the TAG, by the reader's tag_flow, not by a field -- the join is per
-- (rank, tag), never per message. Untagged doors carry the tag inside an AM
-- payload and stay unjoinable."
--
-- CLAUSE 1. If a hardware-tag door fired at all, some UCP send must join some
-- UCT operation. Zero matched flows WITH tag doors present means the two layers
-- disagree about what the tag is -- the join being wrong, not the workload being
-- quiet. Guarded on the tag doors existing, because on MN5's HPC-X 2.16 the
-- ConnectX tag matcher is OFF by default and this job deliberately does not
-- force it on: forcing UCX_RC_MLX5_TM_ENABLE=y to make a table non-empty would
-- be tuning the fabric until the tracer looks good. If there are no tag doors,
-- this clause is silent and the ABSENCE is the finding (see PREDICTION.md P3) --
-- reported by the job script, not by turning a real question into a false pass.
--
-- CLAUSE 2 is 21_ucx_deep's claim, unchanged in shape, run for the first time
-- against a third party's traffic: a matched flow that used ONE door and whose
-- byte totals agree is an eager flow, where one UCP send is one UCT operation by
-- construction. Flows are ALLOWED to differ in general -- a rendezvous flow is
-- one send and one advertisement, a fragmented one is several operations -- so
-- `doors = 1 AND uct_payload = ucp_units` is what selects the eager case
-- WITHOUT naming the rendezvous door.
--
-- NAMES NO TAG. Open MPI's tags encode (context id, MPI tag, source rank) and
-- writing one down would be a golden in a costume.
--
-- BOTH RENDERINGS. `op` decodes to a name in the CSV export and stays an integer
-- in Parquet (QUESTIONS.md), so the tag doors are named as both. 10..13 are
-- tag_eager_short / tag_eager_bcopy / tag_eager_zcopy / tag_rndv_zcopy, which is
-- reconstructed from 21_ucx_deep/asserts/tag_and_addr_are_door_scoped.sql and
-- from no published vocabulary.
SELECT 'a hardware tag door fired but no UCP send joined a wire operation' AS problem,
       NULL AS rank, NULL AS tag, NULL AS ucp_sends, NULL AS uct_ops
WHERE EXISTS (SELECT 1 FROM ucx_tag_send)
  AND EXISTS (SELECT 1 FROM uct_send
              WHERE CAST(op AS TEXT) IN ('10', '11', '12', '13',
                                         'tag_eager_short', 'tag_eager_bcopy',
                                         'tag_eager_zcopy', 'tag_rndv_zcopy'))
  AND NOT EXISTS (SELECT 1 FROM tag_flow WHERE status = 'tag_matched')

UNION ALL

SELECT 'a single-door eager flow disagrees on how many operations it took',
       rank, tag, ucp_sends, uct_ops
FROM tag_flow
WHERE status = 'tag_matched'
  AND CAST(doors AS INTEGER) = 1
  AND CAST(uct_payload AS INTEGER) = CAST(ucp_units AS INTEGER)
  AND CAST(ucp_sends AS INTEGER) <> CAST(uct_ops AS INTEGER);
