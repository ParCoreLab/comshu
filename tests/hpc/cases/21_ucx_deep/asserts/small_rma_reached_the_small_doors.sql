-- requires: transport_mix params.rma_small_bytes
-- A one-sided operation below the zero-copy threshold must take a door that is
-- not zero-copy.
--
-- UCT has fourteen data doors and three of them are a PROTOCOL choice per
-- family: short builds the payload inline in the WQE, bcopy packs it into a
-- bounce buffer, zcopy registers the user buffer and DMAs from it. The first
-- version of this case sent one-sided traffic at rma_bytes only — a megabyte,
-- always zero-copy — so put_short, put_bcopy, get_short and get_bcopy had never
-- been recorded by anything, and the fields their rows would carry had never
-- been checked against hardware.
--
-- DERIVED, not observed: rma_small_bytes is below zcopy_thresh, both are in
-- meta.json, and the job script pins UCX_ZCOPY_THRESH to the second. If the
-- first is under the second then a small put or get cannot legitimately be
-- zero-copy, and a capture where every RMA operation is zcopy means either the
-- small round never ran or the small doors are not instrumented.
--
-- IT DOES NOT SAY WHICH of short or bcopy. That is UCX's choice — it depends on
-- the transport's inline capability, which is a property of the HCA and not of
-- this workload — and asserting either specifically would be asserting the
-- fabric's configuration rather than pcomm's recording.
SELECT 'one-sided traffic below zcopy_thresh took only zero-copy doors' AS problem,
       (SELECT rma_small_bytes FROM params) AS sent,
       (SELECT zcopy_thresh FROM params)    AS threshold
WHERE (SELECT rma_small_bytes FROM params) < (SELECT zcopy_thresh FROM params)
  AND EXISTS (SELECT 1 FROM transport_mix WHERE op IN ('put_zcopy', 'get_zcopy'))
  AND NOT EXISTS (SELECT 1 FROM transport_mix
                  WHERE op IN ('put_short', 'put_bcopy', 'get_short', 'get_bcopy'));
