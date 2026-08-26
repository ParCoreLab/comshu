-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.procgrid_x
-- Every heavy peer is a FACE NEIGHBOUR in the processor grid.
--
-- `processors 2 2 2` in in.lj fixes procgrid; src/procmap.cpp
-- ProcMap::cart_map calls MPI_Cart_create(world, 3, procgrid, periods=1,
-- reorder=0), so coordinates are row-major over (x,y,z) and
--     rank = (cx*py + cy)*pz + cz
-- with reorder=0 guaranteeing the identity permutation.  CommBrick's swap table
-- is built only from procneigh[dim][0/1] (src/comm_brick.cpp:383-396), which
-- MPI_Cart_shift fills with the +-1 neighbour along one axis.  So a heavy edge
-- must differ from its source in EXACTLY ONE coordinate, by exactly +-1 modulo
-- that axis's extent.  Offending rows are heavy edges that are not.
--
-- Division is written as (v - v%m)/m so the numerator is an exact multiple and
-- the expression is identical under sqlite's integer / and DuckDB's real one.
SELECT src_rank, dst_rank, dx, dy, dz
FROM (
  SELECT src_rank, dst_rank,
         CASE WHEN (sx - dxc + px) % px IN (1, px - 1) THEN 1
              WHEN sx = dxc THEN 0 ELSE 9 END AS dx,
         CASE WHEN (sy - dyc + py) % py IN (1, py - 1) THEN 1
              WHEN sy = dyc THEN 0 ELSE 9 END AS dy,
         CASE WHEN (sz - dzc + pz) % pz IN (1, pz - 1) THEN 1
              WHEN sz = dzc THEN 0 ELSE 9 END AS dz
  FROM (
    SELECT src_rank, dst_rank, px, py, pz,
           ((src_rank - src_rank % pz) / pz - ((src_rank - src_rank % pz) / pz) % py) / py AS sx,
           ((src_rank - src_rank % pz) / pz) % py                                          AS sy,
           src_rank % pz                                                                   AS sz,
           ((dst_rank - dst_rank % pz) / pz - ((dst_rank - dst_rank % pz) / pz) % py) / py AS dxc,
           ((dst_rank - dst_rank % pz) / pz) % py                                          AS dyc,
           dst_rank % pz                                                                   AS dzc
    FROM (
      SELECT s.rank AS src_rank, w.rank AS dst_rank,
             (SELECT procgrid_x FROM params) AS px,
             (SELECT procgrid_y FROM params) AS py,
             (SELECT procgrid_z FROM params) AS pz
      FROM ucx_tag_send s
      JOIN ucx_ep_connection c  ON c.rank = s.rank AND c.endpoint = s.endpoint
      JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
      WHERE s.count >= (SELECT halo_min_bytes FROM params)
      GROUP BY 1, 2
    ) e
  ) coords
) d
WHERE dx + dy + dz != 1
ORDER BY src_rank, dst_rank;
