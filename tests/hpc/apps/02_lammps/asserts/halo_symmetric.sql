-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.halo_min_bytes
-- The heavy edge set is symmetric: a -> b heavy implies b -> a heavy.
--
-- Not a statement about bytes, which depend on how many ghost atoms happen to
-- lie in each slab, but about the swap TABLE.  nswap is global
-- (src/comm_brick.cpp:342-351 Allreduces recvneed) and every swap iswap on rank
-- a with sendproc = b has a mirror swap on b with sendproc = a, because
-- sendneed is by construction "recvneed of the neighbour on each side"
-- (src/comm_brick.cpp:246-252).  So the heavy edge relation is symmetric even
-- though its weights are not.
SELECT e.src_rank, e.dst_rank, e.sends
FROM (
  SELECT s.rank AS src_rank, w.rank AS dst_rank, count(*) AS sends
  FROM ucx_tag_send s
  JOIN ucx_ep_connection c  ON c.rank = s.rank AND c.endpoint = s.endpoint
  JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
  WHERE s.count >= (SELECT halo_min_bytes FROM params)
  GROUP BY 1, 2
) e
WHERE NOT EXISTS (
  SELECT 1 FROM (
    SELECT s.rank AS src_rank, w.rank AS dst_rank
    FROM ucx_tag_send s
    JOIN ucx_ep_connection c  ON c.rank = s.rank AND c.endpoint = s.endpoint
    JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
    WHERE s.count >= (SELECT halo_min_bytes FROM params)
    GROUP BY 1, 2
  ) m WHERE m.src_rank = e.dst_rank AND m.dst_rank = e.src_rank
)
ORDER BY e.src_rank, e.dst_rank;
