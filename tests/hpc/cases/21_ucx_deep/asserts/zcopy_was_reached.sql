-- requires: transport_mix params.zcopy_thresh
-- A message above the zero-copy threshold must have taken a zero-copy door.
--
-- This is the first hardware evidence for the family at all. Job 45113099 ran
-- 295 UCT operations of which four were am_zcopy and none were anything else
-- zero-copy, so `iovcnt` — the field that separates one 64 KB buffer from 64
-- scattered ones — had effectively never been written by a real transfer.
--
-- DERIVED, not observed: the workload sends msg_zcopy_bytes, the job script
-- pins UCX_ZCOPY_THRESH to zcopy_thresh, and both numbers are in meta.json. If
-- the first is at least the second, UCX registers the user buffer and DMAs from
-- it, and a capture with no zcopy row means either the sweep never crossed the
-- threshold it was built to cross or the doors are not instrumented. Naming
-- both operands is what makes those two distinguishable.
SELECT 'no zero-copy operation, though msg_zcopy_bytes >= zcopy_thresh' AS problem,
       (SELECT msg_zcopy_bytes FROM params) AS sent,
       (SELECT zcopy_thresh FROM params)    AS threshold
WHERE (SELECT msg_zcopy_bytes FROM params) >= (SELECT zcopy_thresh FROM params)
  AND NOT EXISTS (SELECT 1 FROM transport_mix
                  WHERE op IN ('am_zcopy', 'put_zcopy', 'get_zcopy',
                               'tag_eager_zcopy', 'tag_rndv_zcopy'));
