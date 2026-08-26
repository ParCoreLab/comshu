-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.halo_peers_per_rank
-- Each rank's heavy traffic goes to exactly halo_peers_per_rank peers.
--
-- src/comm_brick.cpp:351 sets nswap = 2*(maxneed[0]+maxneed[1]+maxneed[2]);
-- for this box maxneed = 1 in every dimension (cutghost 2.8, prd 67.19,
-- procgrid 2: int(2.8*2/67.19)+1 = 1), so six swaps.  With procgrid[d] = 2 the
-- two neighbours in dimension d are the SAME rank -- MPI_Cart_shift on a
-- periodic axis of length 2 returns the same peer both ways -- so six swaps
-- reach three distinct peers, and CommBrick reaches nobody else: sendproc[] is
-- filled only from procneigh[dim][0/1] (src/comm_brick.cpp:383-396).
--
-- DRIVEN OFF THE SENDS, NOT OFF THE EDGES.  A version that grouped the joined
-- edges and tested the group count passes VACUOUSLY when the peer join
-- resolves nothing at all, which is exactly the state this stack is in (see
-- peer_rank_resolvable.sql).  Every rank that sent a halo-sized message owes
-- exactly halo_peers_per_rank distinct peers, and a rank that owes them and
-- resolves none is a row here rather than an absence.
SELECT s.rank AS src_rank,
       count(DISTINCT w.rank) AS resolved_peers,
       count(*)               AS heavy_sends
FROM ucx_tag_send s
LEFT JOIN ucx_ep_connection  c ON c.rank = s.rank AND c.endpoint = s.endpoint
LEFT JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
WHERE s.count >= (SELECT halo_min_bytes FROM params)
GROUP BY s.rank
HAVING count(DISTINCT w.rank) != (SELECT halo_peers_per_rank FROM params)
ORDER BY s.rank;
