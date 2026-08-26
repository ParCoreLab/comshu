-- requires: ucx_tag_send params.halo_peers_per_rank
-- Each rank carries its halo over exactly halo_peers_per_rank UCX endpoints.
--
-- This is halo_peer_fanout's claim, made without the cross-rank join that
-- cannot presently be completed on this stack (see peer_rank_resolvable).  An
-- endpoint is a peer: OpenMPI's pml_ucx creates one ucp_ep per remote process
-- (ompi/mca/pml/ucx/pml_ucx.c mca_pml_ucx_add_proc_common) and reuses it for
-- every message to that process, so distinct endpoints carrying halo-sized
-- traffic are distinct halo peers even when their rank numbers are unknown.
--
-- The oracle is LAMMPS's own: src/comm_brick.cpp:351 gives
-- nswap = 2*(maxneed[0]+maxneed[1]+maxneed[2]) = 6 for this box (cutghost 2.8,
-- prd 67.19, procgrid[d] = 2 => maxneed[d] = int(2.8*2/67.19)+1 = 1), and
-- src/comm_brick.cpp:383-396 fills sendproc[] only from procneigh[dim][0/1].
-- On a periodic axis of length 2 MPI_Cart_shift returns the SAME rank in both
-- directions, so those six swaps address three distinct peers -- not six.
SELECT s.rank, count(DISTINCT s.endpoint) AS halo_endpoints
FROM ucx_tag_send s
WHERE s.count >= (SELECT halo_min_bytes FROM params)
GROUP BY s.rank
HAVING count(DISTINCT s.endpoint) != (SELECT halo_peers_per_rank FROM params)
ORDER BY s.rank
