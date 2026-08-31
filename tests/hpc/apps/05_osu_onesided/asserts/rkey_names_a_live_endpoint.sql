-- requires: ucx_rkey ucx_endpoint
-- A remote key is unpacked ON AN ENDPOINT. That endpoint must be one something
-- opened.
--
-- WHY THIS MATTERS MORE FOR ONE-SIDED THAN FOR ANYTHING ELSE. In a tagged send
-- the row itself names the endpoint, so the path from traffic to peer is one
-- hop. A one-sided row names a KEY, and the key names the endpoint, and only
-- then does ucx_ep_connection name the peer worker. Three hops, and this is the
-- middle one. If it is broken, every put in the capture is unattributable to a
-- peer even though nothing about the put looks wrong.
--
-- WHAT IT CATCHES that every_rma_key_resolves does not: a key recorded with a
-- stale or wrongly-read endpoint handle. `ucp_ep_rkey_unpack` takes the
-- endpoint as its first argument, so reading the wrong argument slot yields a
-- plausible integer that joins to nothing — the characteristic failure of
-- hooking a function by signature, and one that no count and no byte total can
-- see.
--
-- Names no endpoint and no rank; it asserts referential integrity between two
-- tables the capture wrote itself.
SELECT 'an rkey names an endpoint that no ucx_endpoint row opened' AS problem,
       k.rank                                                      AS rank,
       k.endpoint                                                  AS orphan_endpoint,
       count(*)                                                    AS rkey_rows
FROM ucx_rkey k
WHERE k.endpoint <> 0
  AND NOT EXISTS (SELECT 1 FROM ucx_endpoint e
                  WHERE e.rank = k.rank AND e.endpoint = k.endpoint)
GROUP BY k.rank, k.endpoint;
