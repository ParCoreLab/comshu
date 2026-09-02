-- requires: comm_edges params.px params.py params.pz params.ranks params.x_share_bp params.y_share_bp params.z_share_bp params.share_tol_bp
-- Each of a rank's four peers must carry the share of its bytes that LAMMPS's
-- ghost-slab geometry says it must -- and the four shares are three different
-- numbers, so this cannot be satisfied by a capture that merely found four
-- peers.
--
-- THE ORACLE, DERIVED IN FULL IN PREDICTION-r2.md §3 AND NEEDING NO MEASUREMENT.
-- comm_brick.cpp:383-396 acquires ghosts one axis at a time and forwards the
-- ghosts acquired in earlier axes, so with subdomain extents lx,ly,lz, ghost
-- cutoff c and Lx = lx+2c the six slabs are
--
--     x swaps (2) : c.ly.lz    y swaps (2) : Lx.c.lz    z swaps (2) : Lx.Ly.c
--
-- With `processors 2 2 4` on a cubic box of side L: lx = ly = L/2, lz = L/4,
-- c = cutneighmax = 2.5 + 0.3 = 2.8, L = 40 . (4/0.8442)^(1/3) = 67.1838. The x
-- and y axes have procgrid 2, so BOTH of their swaps land on ONE peer; the z
-- axis has procgrid 4, so its two swaps land on two DIFFERENT peers. Hence
--
--     r^1  : 2 x 1579.78 / 15447.5 = 20.45 %       (x peer)
--     r^2  : 2 x 1843.15 / 15447.5 = 23.86 %       (y peer)
--     r+-4 : 1 x 4300.81 / 15447.5 = 27.84 % each  (z peers)
--
-- Atom density and per-atom payload cancel out of every ratio, which is why
-- this is a share and not a byte count, and why it does not care about the step
-- count or the reneighbour period.
--
-- WHAT IT CATCHES. Bytes attributed to the right rank pair but the wrong one of
-- them; a capture that resolves peers by hashing rather than by identity and so
-- permutes the four; a UCP-to-edge accounting that drops one of the two swaps
-- on a folded axis (that would put x at 10 % and z at 33 %); and any of the
-- protocol-level asymmetries -- eager on one lane and rendezvous on the other --
-- that could bill a shared-memory transfer differently from an InfiniBand one.
-- That last is the interesting one, because in the `block` arm the x and y peers
-- are on shared memory and the z peers are on the fabric, while in the `cyclic`
-- arm it is the other way round, and the SAME four shares must come out both
-- times.
--
-- THE Z PEERS ARE ALSO CHECKED AGAINST EACH OTHER. Every subdomain is the same
-- size, so the two z slabs are equal by symmetry; clause 4 asserts that without
-- reference to any predicted value at all.
--
-- Tolerance is share_tol_bp basis points (1 bp = 0.01 %), wide enough for the
-- collective and wireup residue and much narrower than the 3.4-point gap
-- between the x and y shares -- so a permutation of the four is caught.
WITH coord AS (
  SELECT i AS rank, i % p.px AS ci, (i // p.px) % p.py AS cj, i // (p.px * p.py) AS ck,
         p.px, p.py, p.pz
  FROM range(0, 1024) t(i), params p WHERE i < p.ranks
), axis AS (
  SELECT rank, ((ci + 1) % px) + px * cj + px * py * ck AS peer, 'x' AS ax FROM coord
  UNION ALL SELECT rank, ci + px * ((cj + 1) % py) + px * py * ck, 'y' FROM coord
  UNION ALL SELECT rank, ci + px * cj + px * py * ((ck + 1) % pz), 'z+' FROM coord
  UNION ALL SELECT rank, ci + px * cj + px * py * ((ck + pz - 1) % pz), 'z-' FROM coord
), tot AS (
  SELECT src_rank, sum(CAST(bytes AS BIGINT)) AS total FROM comm_edges GROUP BY src_rank
), edge AS (
  SELECT src_rank, dst_rank, sum(CAST(bytes AS BIGINT)) AS bytes
  FROM comm_edges GROUP BY src_rank, dst_rank
), share AS (
  SELECT a.rank, a.ax, a.peer, COALESCE(e.bytes, 0) AS bytes, t.total,
         (10000 * COALESCE(e.bytes, 0)) / t.total AS bp,
         CASE a.ax WHEN 'x' THEN (SELECT x_share_bp FROM params)
                   WHEN 'y' THEN (SELECT y_share_bp FROM params)
                   ELSE          (SELECT z_share_bp FROM params) END AS want_bp
  FROM axis a
  JOIN tot  t ON t.src_rank = a.rank
  LEFT JOIN edge e ON e.src_rank = a.rank AND e.dst_rank = a.peer
  WHERE a.peer <> a.rank
)
SELECT 'a peer carries a share the ghost-slab geometry does not allow' AS problem,
       rank, ax, peer, bytes, bp, want_bp
FROM share
WHERE bp < want_bp - (SELECT share_tol_bp FROM params)
   OR bp > want_bp + (SELECT share_tol_bp FROM params)

UNION ALL

-- The two z peers must carry EQUAL volumes. No predicted number enters this
-- clause: it is pure symmetry of the decomposition, and it survives any error
-- in the constants above.
SELECT 'the two z peers carry unequal volumes', a.rank, 'z', b.peer,
       a.bytes, b.bytes, 0
FROM (SELECT rank, peer, bytes FROM share WHERE ax = 'z+') a
JOIN (SELECT rank, peer, bytes FROM share WHERE ax = 'z-') b ON b.rank = a.rank
WHERE a.bytes + b.bytes > 0
  AND (100 * (CASE WHEN a.bytes > b.bytes THEN a.bytes - b.bytes ELSE b.bytes - a.bytes END))
      > 10 * (a.bytes + b.bytes) / 2

UNION ALL

-- THE ANTI-VACUITY GUARD, and it is needed because `share` reaches comm_edges
-- through an INNER join against the per-rank total: with comm_edges empty, `tot`
-- is empty, `share` is empty, and both clauses above report no offending rows
-- while checking nothing. That is the failure mode round 1 of this case shipped
-- three times over an empty peer-resolved relation (NOTES.md #7), and the same
-- shape that let network_share_of_wire_bytes.sql pass on an empty transport_mix
-- until it was caught.
SELECT 'comm_edges is empty, so this assertion checked nothing', -1, '-', -1, 0, 0, 0
WHERE (SELECT count(*) FROM comm_edges) = 0;
