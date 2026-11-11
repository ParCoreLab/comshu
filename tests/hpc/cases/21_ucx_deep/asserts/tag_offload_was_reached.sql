-- requires: transport_mix params.tm_thresh
-- The hardware tag matcher must have been used, since the run asked for it.
--
-- THIS IS THE ONE ASSERTION HERE THAT IS AN EXPERIMENT. The others state
-- properties of the tracer; this states a property of the fabric plus the
-- tracer, and the two failure modes are different:
--   * comshu does not instrument the tag doors — a defect, ours;
--   * ConnectX tag matching did not engage on this stack — a fact about MN5,
--     and the job's tm_off arm is the control that tells them apart.
--
-- The job script sets UCX_RC_MLX5_TM_ENABLE=y and UCX_DC_MLX5_TM_ENABLE=y (which
-- of the two UCX picks is its decision, not ours) and pins UCX_TM_THRESH to
-- tm_thresh, and three of the four sweep sizes are above it. So if offload is
-- available at all, these doors fire.
--
-- If this fails while the tm_off arm shows the same zero, the offload never
-- engaged and the honest outcome is an expect_fail_asserts entry citing the
-- evidence — NOT a relaxed predicate. An assertion edited until it passes is a
-- golden wearing a costume, and this suite has the comment about that at the top
-- of sql_check.sh.
SELECT 'no hardware-tag-matching door, though three sweep sizes exceed tm_thresh' AS problem,
       (SELECT tm_thresh FROM params)       AS tm_thresh,
       (SELECT msg_zcopy_bytes FROM params) AS smallest_eligible
WHERE (SELECT msg_zcopy_bytes FROM params) >= (SELECT tm_thresh FROM params)
  AND NOT EXISTS (SELECT 1 FROM transport_mix
                  WHERE op IN ('tag_eager_short', 'tag_eager_bcopy',
                               'tag_eager_zcopy', 'tag_rndv_zcopy'));
