-- requires: comm_edges params.px params.py params.pz params.ranks
-- The MESSAGE counts must split 2 : 2 : 1 : 1 across the four peers, while the
-- BYTES split 20.45 : 23.86 : 27.84 : 27.84. Two different ratios, both derived
-- from the same six lines of LAMMPS, and a capture has to satisfy both.
--
-- WHY THIS IS WORTH A SEPARATE FILE. per_peer_byte_share_matches_the_geometry
-- checks volume; this checks cardinality, and they disagree on which peer is
-- biggest. The x peer receives a THIRD of a rank's halo messages and only a
-- FIFTH of its bytes; the z peers receive a SIXTH of the messages and MORE than
-- a fifth of the bytes each. A tracer that counted messages correctly and
-- attributed bytes by dividing a rank total evenly, or one that got the bytes
-- right off the wire and synthesised a plausible message count, fails exactly
-- one of these two files. Together they are much harder to satisfy by accident
-- than either alone.
--
-- THE ORACLE. comm_brick.cpp:342-396. nswap = 2*(maxneed[0]+maxneed[1]+
-- maxneed[2]) and for this box every maxneed is 1, so six swaps: two per axis,
-- one to procneigh[dim][0] and one to procneigh[dim][1]. With procgrid = 2 on
-- x and y those two are the SAME rank (procmap.cpp:437-443, grid_shift), so the
-- x and y peers are each addressed by two swaps per communication call and each
-- z peer by one. Every call -- forward_comm each step, borders at each
-- reneighbour -- walks the same swap table in the same order, so the ratio holds
-- for the total however many calls there were, and the step count enters
-- nothing here.
--
-- STATED AS A RATIO AGAINST THE Z PEERS rather than as absolute counts, so that
-- nothing depends on the number of steps, on how many messages an exchange or a
-- border call adds, or on whether Kokkos's newton-off setting removes the
-- reverse-comm population (it does, and that removal is uniform across swaps).
-- Tolerance +-10 %.
WITH coord AS (
  SELECT i AS rank, i % p.px AS ci, (i // p.px) % p.py AS cj, i // (p.px * p.py) AS ck,
         p.px, p.py, p.pz
  FROM range(0, 1024) t(i), params p WHERE i < p.ranks
), axis AS (
  SELECT rank, ((ci + 1) % px) + px * cj + px * py * ck AS peer, 'x' AS ax FROM coord
  UNION ALL SELECT rank, ci + px * ((cj + 1) % py) + px * py * ck, 'y' FROM coord
  UNION ALL SELECT rank, ci + px * cj + px * py * ((ck + 1) % pz), 'z+' FROM coord
  UNION ALL SELECT rank, ci + px * cj + px * py * ((ck + pz - 1) % pz), 'z-' FROM coord
), edge AS (
  SELECT src_rank, dst_rank, sum(CAST(sends AS BIGINT)) AS sends
  FROM comm_edges GROUP BY src_rank, dst_rank
), n AS (
  SELECT a.rank, a.ax, a.peer, COALESCE(e.sends, 0) AS sends
  FROM axis a LEFT JOIN edge e ON e.src_rank = a.rank AND e.dst_rank = a.peer
  WHERE a.peer <> a.rank
), zbase AS (
  SELECT rank, sum(sends) / 2 AS zavg FROM n WHERE ax IN ('z+','z-') GROUP BY rank
)
SELECT 'a folded axis does not carry twice a z peer''s message count' AS problem,
       n.rank, n.ax, n.peer, n.sends, z.zavg
FROM n JOIN zbase z ON z.rank = n.rank
WHERE n.ax IN ('x','y') AND z.zavg > 0
  AND (n.sends * 10 < 2 * z.zavg * 9 OR n.sends * 10 > 2 * z.zavg * 11)

UNION ALL

SELECT 'the two z peers do not carry equal message counts',
       a.rank, 'z', b.peer, a.sends, b.sends
FROM (SELECT rank, peer, sends FROM n WHERE ax = 'z+') a
JOIN (SELECT rank, peer, sends FROM n WHERE ax = 'z-') b ON b.rank = a.rank
WHERE a.sends + b.sends > 0
  AND (10 * (CASE WHEN a.sends > b.sends THEN a.sends - b.sends ELSE b.sends - a.sends END))
      > (a.sends + b.sends) / 2

UNION ALL

-- Guard against the whole thing being vacuous: if no z edge carries any message
-- the two clauses above have nothing to compare and both pass silently.
SELECT 'no z-axis edge carries any message', rank, 'z', 0, 0, 0
FROM zbase WHERE zavg = 0;
