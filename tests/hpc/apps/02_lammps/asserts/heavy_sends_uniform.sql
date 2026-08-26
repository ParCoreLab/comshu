-- requires: ucx_tag_send params.halo_min_bytes
-- Every rank issues the SAME number of halo-sized messages.
--
-- nswap is a global quantity, not a local one: src/comm_brick.cpp:342-351
-- MPI_Allreduces recvneed to a common maxneed[] and every rank then sets
-- nswap = 2*(maxneed[0]+maxneed[1]+maxneed[2]).  CommBrick::forward_comm
-- (src/comm_brick.cpp:497) and CommKokkos::forward_comm_device
-- (src/KOKKOS/comm_kokkos.cpp:150) both walk that same loop once per timestep
-- on every rank, and with periodic boundaries and procgrid[d] = 2 no rank ever
-- takes the sendproc == me branch.  So the COUNT of halo messages is identical
-- on all ranks even though their SIZES are not -- the sizes depend on how many
-- ghost atoms happen to lie in each slab, the count does not.
--
-- This is the assertion that survives the peer-attribution gap: it needs only
-- ucx_tag_send, never the ep_connection -> worker_address join.
SELECT s.rank, count(*) AS heavy_sends
FROM ucx_tag_send s
WHERE s.count >= (SELECT halo_min_bytes FROM params)
GROUP BY s.rank
HAVING count(*) != (
  SELECT min(n) FROM (
    SELECT count(*) AS n FROM ucx_tag_send t
    WHERE t.count >= (SELECT halo_min_bytes FROM params)
    GROUP BY t.rank
  ) x
)
ORDER BY s.rank
