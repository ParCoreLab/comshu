-- requires: ucx_rkey_pack ucx_mem_map params.n_windows
-- The other side of the key. A registration that was exported must be one that
-- was mapped.
--
-- NOBODY HAS CHECKED THIS JOIN. `ucx_rkey` is the IMPORT side — the origin
-- unpacking a peer's key — and it is what case 22 asserted. `ucx_rkey_pack` is
-- the EXPORT side: this rank made its own registration remotely accessible.
-- Its `memh` is a ucp_mem_h, the same handle ucx_mem_map opens, and the two
-- records have never been joined by any assertion in this repository.
--
-- WHY THE JOIN MUST HOLD. MPI_Win_allocate maps the window's memory through
-- ucp_mem_map and then exports it with ucp_rkey_pack; you cannot pack a handle
-- that was never mapped. A ucx_rkey_pack row whose memh resolves to nothing is
-- an artifact about an entity that does not exist in the capture — the same
-- shape of defect ucx_rkey was created to fix on the import side, one door
-- over. It would mean the export path is hooked and the map path is not, and
-- the capture would be unable to say WHAT was exported: the pack row carries a
-- buffer hash and a length, and the address and extent live only in mem_map.
--
-- SECOND CLAUSE: this workload creates n_windows windows, each of which must be
-- exported for the peer to reach it, so a rank that packed nothing at all while
-- the run demonstrably did RMA has lost the whole export door. Asserted per
-- rank that packed anything, and as a floor for the same reason as
-- a_key_was_unpacked_for_every_window: OpenMPI packs its state window too.
SELECT 'a packed rkey names a memh that no ucx_mem_map row created' AS problem,
       p.rank                                                       AS rank,
       p.memh                                                       AS orphan_memh,
       count(*)                                                     AS pack_rows
FROM ucx_rkey_pack p
WHERE p.memh <> 0
  AND NOT EXISTS (SELECT 1 FROM ucx_mem_map m
                  WHERE m.rank = p.rank AND m.memh = p.memh)
GROUP BY p.rank, p.memh
UNION ALL
SELECT 'a rank packed fewer keys than the workload created windows',
       q.rank,
       (SELECT count(*) FROM ucx_rkey_pack p2 WHERE p2.rank = q.rank),
       (SELECT n_windows FROM params)
FROM (SELECT DISTINCT rank FROM ucx_rkey_pack) q
WHERE (SELECT count(*) FROM ucx_rkey_pack p2 WHERE p2.rank = q.rank)
      < (SELECT n_windows FROM params);
