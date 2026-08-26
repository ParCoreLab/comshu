-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.halo_min_bytes
-- LAMMPS never sends its own halo to itself.
--
-- src/comm_brick.cpp:497 and src/KOKKOS/comm_kokkos.cpp:152 both guard the MPI
-- path with `if (sendproc[iswap] != me)` and take a local pack/unpack copy
-- otherwise, so a self-directed message above the halo threshold cannot come
-- from the application.  A self edge here is either the qp_num/endpoint
-- collision signature link_health.self_edges exists to name, or a peer
-- misattribution in the prefix-hash join.
SELECT s.rank AS src_rank, w.rank AS dst_rank,
       count(*) AS sends, sum(s.count) AS bytes
FROM ucx_tag_send s
JOIN ucx_ep_connection c  ON c.rank = s.rank AND c.endpoint = s.endpoint
JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
WHERE s.count >= (SELECT halo_min_bytes FROM params)
  AND w.rank = s.rank
GROUP BY 1, 2
ORDER BY 1;
