-- requires: rank_identity ucx_tag_send params.gpus
-- A capture that records a device ordinal on every send must not present a
-- rank whose device is unknown.
--
-- THIS ASSERTION IS PREDICTED TO FAIL, IN WRITING, BEFORE THE RUN. A sibling
-- study reports that rank_identity.device is NULL and label reads "GPU ?"
-- whenever the gpumem/NVBit provider was not loaded, because the device is
-- derived only from NVBit records. I have not verified that and I am not
-- marking this expect_fail on another agent's word -- meta.json's
-- expect_fail_asserts is for a defect with evidence, and predicting the failure
-- here is the honest substitute. If it passes, the sibling's claim does not
-- generalise to a ucx-only capture and that is worth knowing too.
--
-- WHY QE IS THE RIGHT INSTRUMENT FOR IT. comshu.fbs type 606 gives UcxTagSend a
-- `device:uint16` field, and pw.x is built with -D__GPU_MPI (install/m4/
-- x_ac_qe_cuda.m4:92-93) so it hands MPI device pointers -- round 1 proved that
-- the expensive way, with rc_verbs aborting on
-- `ibv_reg_mr(...) failed: Bad address / md supports: host`. So this capture
-- carries a device ordinal on order 10^5 rows. If rank_identity still cannot
-- name a device, the information is present in the capture and the derived table
-- declines to use it, which is a sharper claim than "a column is null".
--
-- THE JOB ASKS FOR --gres=gpu:4 WITH FOUR RANKS PER NODE FOR THIS ASSERTION.
-- Round 1 ran one GPU per node, so CUDA_VISIBLE_DEVICES made every rank device 0
-- and a device column that always said 0 was indistinguishable from one that
-- said nothing. With four visible GPUs the ordinals must actually differ across
-- the ranks of a node, so `device` has to carry information to be right.
--
-- CLAUSE 2 does not test NULLness at all: it tests that the ordinals are in
-- range, which is suite/asserts/owner_in_range's claim. A device >= gpus is the
-- phantom-ordinal defect 04_nvshmem carries, and it costs one scan to look.
--
-- Names no rank and no ordinal; `gpus` is per node and comes from meta-r2.json.
SELECT 'a rank has no device, though tagged sends in this capture carry one' AS problem,
       ri.rank                                            AS rank,
       ri.device                                          AS device,
       ri.label                                           AS label,
       (SELECT count(*) FROM ucx_tag_send)                AS sends_carrying_a_device
FROM rank_identity ri
WHERE EXISTS (SELECT 1 FROM ucx_tag_send)
  AND (ri.device IS NULL OR CAST(ri.device AS TEXT) = '')

UNION ALL

SELECT 'a rank names a device ordinal that does not exist on its node',
       ri.rank, ri.device, ri.label, (SELECT gpus FROM params)
FROM rank_identity ri
WHERE ri.device IS NOT NULL
  AND CAST(ri.device AS TEXT) <> ''
  AND CAST(ri.device AS INTEGER) >= (SELECT gpus FROM params);
