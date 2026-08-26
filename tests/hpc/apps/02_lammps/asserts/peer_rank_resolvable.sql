-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.halo_min_bytes
-- Every halo-sized UCX message must name the rank it went to.
--
-- This is the weakest possible form of "the capture is a communication trace":
-- not how many bytes, not to whom in particular, only that a peer EXISTS in the
-- record.  The chain is the one the record model provides and the only one that
-- crosses a rank boundary (providers/ucx/ucx_records.hpp:275-294):
--     ucx_tag_send.endpoint
--       -> ucx_ep_connection.remote_prefix_hash          (per rank)
--       -> ucx_worker_address.prefix_hash                (another rank's row)
--       -> that row's rank
--
-- Offending rows = halo-sized sends whose peer rank cannot be named.
SELECT s.rank AS src_rank, s.endpoint, count(*) AS unresolved_heavy_sends,
       sum(s.count) AS unresolved_bytes
FROM ucx_tag_send s
LEFT JOIN ucx_ep_connection  c ON c.rank = s.rank AND c.endpoint = s.endpoint
LEFT JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
WHERE s.count >= (SELECT halo_min_bytes FROM params)
  AND w.rank IS NULL
GROUP BY 1, 2
ORDER BY unresolved_bytes DESC;
