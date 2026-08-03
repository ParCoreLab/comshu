-- requires: ib_mem_region ib_queue_pair ib_modify_qp params.ranks
-- Every rank runs the same NVSHMEM bootstrap, so every rank must register the
-- same number of regions, create the same number of QPs, and drive the same
-- number of transitions. Asymmetry means a rank's registrations stopped being
-- captured — which is the failure this case exists to catch, and the one a
-- total cannot show, because a count that is too low still looks like a count.
--
-- Deliberately NOT the absolute numbers. Those are allocation-dependent, and we
-- have the evidence: at the *same commit* (5a28119), job 44073877 on rack
-- as07r3b recorded 115 regions per rank and job 44113361 on as04r5b recorded
-- 114 — while queue_pair (108) and modify_qp (132) were identical on both. Six
-- further runs on as04r5b all give 114. The old ib_entities golden pinned 460
-- and went red on hardware, having tested nothing about the tool.
--
-- Why regions in particular drift: ib_entities counts reg_mr *calls*, not live
-- regions. Freed pointers get re-registered and both are recorded, because
-- dereg emits no close record yet — so the number tracks NVSHMEM's allocator
-- churn. Symmetry survives that; the absolute count does not.
SELECT * FROM (
  SELECT 'ib_mem_region' AS entity,
         min(n) AS lo, max(n) AS hi, count(*) AS ranks_seen
  FROM (SELECT rank, count(*) AS n FROM ib_mem_region GROUP BY rank)
  UNION ALL
  SELECT 'ib_queue_pair', min(n), max(n), count(*)
  FROM (SELECT rank, count(*) AS n FROM ib_queue_pair GROUP BY rank)
  UNION ALL
  SELECT 'ib_modify_qp', min(n), max(n), count(*)
  FROM (SELECT rank, count(*) AS n FROM ib_modify_qp GROUP BY rank)
)
WHERE lo != hi                              -- a rank captured fewer than its peers
   OR lo = 0                                -- or nothing at all
   OR ranks_seen != (SELECT ranks FROM params);   -- or a rank missing entirely
