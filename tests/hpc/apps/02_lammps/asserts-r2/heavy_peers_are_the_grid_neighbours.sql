-- requires: comm_edges ucx_tag_send params.px params.py params.pz params.ranks params.heavy_share_pct params.fanout
-- The rank-space topology must be the one LAMMPS's own processor map defines.
--
-- THE ORACLE, AND IT IS A FUNCTION IN LAMMPS'S SOURCE. The deck says
-- `processors 2 2 4 map xyz`, so src/procmap.cpp:409-437 (ProcMap::xyz_map,
-- xyz == "xyz") assigns
--
--     grid2proc[i][j][k] = k*procgrid[1]*procgrid[0] + j*procgrid[0] + i
--
-- i.e. rank r has grid coordinates (r % px, (r/px) % py, r/(px*py)), and
-- procmap.cpp:437-453 takes its six neighbours by grid_shift on each axis with
-- periodic wrap. The CTE below is that formula transcribed, and NOTHING ELSE:
-- no rank number the run produced appears anywhere in this file, only px, py, pz
-- and ranks out of params.
--
-- The six neighbours collapse to FOUR distinct peers here because px = py = 2
-- makes the plus and minus neighbour on those axes the same rank -- which is why
-- `fanout` is a parameter and not the literal 6.
--
-- WHAT IT CATCHES, and it is three different defects with one query:
--   * a peer resolution that lands on the wrong rank (clause 1). On the HPC-X
--     stack this join runs through ucx_ep_connection.remote_prefix_hash ->
--     ucx_worker_address.prefix_hash, and 01_gromacs round 1 measured that join
--     resolving to the WRONG worker on Open MPI 4.1.x -- present, populated,
--     and naming ranks that never spoke. A hash collision or a stale address
--     would look exactly like that and would fail here.
--   * a peer that should carry a fifth of a rank's traffic and carries none
--     (clause 2). Driven off the neighbour set, which exists whether or not the
--     capture does, so a missing edge produces a row rather than an absence.
--   * a fan-out that is right in cardinality and wrong in membership -- clause 1
--     and clause 2 together pin the set, not just its size.
--
-- HEAVY IS RELATIVE, ON PURPOSE. An edge counts if it carries at least
-- heavy_share_pct of its source rank's edge bytes. The four neighbour edges are
-- predicted at 20.5 / 23.9 / 27.8 / 27.8 % (PREDICTION-r2.md P2.1) and the
-- residual population -- Open MPI's collectives behind LAMMPS's thermo
-- Allreduce, and UCX wireup -- is predicted under 1 % in total. No byte count
-- is named, so this assertion is portable to a different box size or step count.
WITH universe AS (
  SELECT i AS rank FROM range(0, 1024) t(i), params p WHERE i < p.ranks
), coord AS (
  SELECT u.rank, u.rank % p.px AS i, (u.rank // p.px) % p.py AS j,
         u.rank // (p.px * p.py) AS k, p.px, p.py, p.pz
  FROM universe u, params p
), nb AS (
  SELECT rank, ((i + 1) % px)      + px * j                  + px * py * k AS peer FROM coord
  UNION SELECT rank, ((i + px - 1) % px) + px * j             + px * py * k FROM coord
  UNION SELECT rank, i + px * ((j + 1) % py)                  + px * py * k FROM coord
  UNION SELECT rank, i + px * ((j + py - 1) % py)             + px * py * k FROM coord
  UNION SELECT rank, i + px * j + px * py * ((k + 1) % pz)                  FROM coord
  UNION SELECT rank, i + px * j + px * py * ((k + pz - 1) % pz)             FROM coord
), neigh AS (
  SELECT rank, peer FROM nb WHERE peer <> rank
), tot AS (
  SELECT src_rank, sum(CAST(bytes AS BIGINT)) AS total FROM comm_edges GROUP BY src_rank
), heavy AS (
  SELECT e.src_rank, e.dst_rank, sum(CAST(e.bytes AS BIGINT)) AS bytes, t.total
  FROM comm_edges e JOIN tot t ON t.src_rank = e.src_rank
  GROUP BY e.src_rank, e.dst_rank, t.total
  HAVING sum(CAST(e.bytes AS BIGINT)) * 100 >= t.total * (SELECT heavy_share_pct FROM params)
)
SELECT 'a heavy edge names a rank that is not a grid neighbour' AS problem,
       h.src_rank, h.dst_rank, h.bytes, (100 * h.bytes) / h.total AS pct
FROM heavy h
LEFT JOIN neigh n ON n.rank = h.src_rank AND n.peer = h.dst_rank
WHERE n.peer IS NULL

UNION ALL

SELECT 'a grid neighbour carries no heavy edge',
       n.rank, n.peer, COALESCE(h.bytes, 0), 0
FROM neigh n
LEFT JOIN heavy h ON h.src_rank = n.rank AND h.dst_rank = n.peer
WHERE h.dst_rank IS NULL

UNION ALL

-- The cardinality, stated separately so that a capture which resolved four
-- edges per rank onto four WRONG peers and a capture which resolved three onto
-- three right ones are told apart in the output rather than in the head.
SELECT 'a rank has the wrong number of heavy peers',
       src_rank, count(*), 0, 0
FROM heavy GROUP BY src_rank
HAVING count(*) <> (SELECT fanout FROM params);
