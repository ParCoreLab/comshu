-- requires: transport_mix params.rndv_thresh
-- The rendezvous door ADVERTISES. It must never report outbound payload.
--
-- `TagRndvZcopy` is the one door in the family where `payload` is bytes that did
-- NOT leave this node: the call offers a buffer for the peer to pull and puts
-- only `header` bytes on the wire. The derived layer splits it into
-- advertised_bytes for exactly that reason — folding it into out_bytes bills the
-- same buffer twice, once as this rank's send and again as the peer's Get.
--
-- Two claims, and they fail differently:
--   * a rendezvous operation reporting out_bytes > 0 is the split collapsing;
--   * no rendezvous operation at all, when the sweep sends msg_rndv_bytes and
--     the script pins UCX_RNDV_THRESH to rndv_thresh, is the door not being
--     reached.
-- The second is the one no capture in this project has ever satisfied, which is
-- why it is asserted from the params rather than from anything observed.
SELECT 'a rendezvous operation reported outbound payload' AS problem,
       rank, tl_name, ops, out_bytes, advertised_bytes
FROM transport_mix
WHERE op = 'tag_rndv_zcopy' AND CAST(out_bytes AS INTEGER) > 0
UNION ALL
SELECT 'no rendezvous operation, though msg_rndv_bytes >= rndv_thresh',
       NULL, NULL,
       (SELECT msg_rndv_bytes FROM params),
       (SELECT rndv_thresh FROM params),
       NULL
WHERE (SELECT msg_rndv_bytes FROM params) >= (SELECT rndv_thresh FROM params)
  AND NOT EXISTS (SELECT 1 FROM transport_mix WHERE op = 'tag_rndv_zcopy');
