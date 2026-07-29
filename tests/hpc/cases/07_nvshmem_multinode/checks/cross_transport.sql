-- FUTURE ORACLE (case is xfail; no golden until the NVBit multinode deadlock is
-- fixed). The invariant this case exists to prove: in ONE execution, the ring's
-- intra-node hops appear as NVLink peer traffic and its inter-node hops as IB
-- traffic, on the same rank space.
--
-- Expected shape at 8 PEs: the two cross-node senders (PE3->PE4, PE7->PE0) carry
-- the IB sends, and the remaining six hops carry the NVLink stores. Asserting
-- both columns in one row per rank is only meaningful because a single execution
-- produced both — under replay these are different runs.
SELECT r.rank,
       (SELECT count(*) FROM gpu_access   g WHERE g.rank = r.rank) AS nvlink_accesses,
       (SELECT count(*) FROM ib_post_send s WHERE s.rank = r.rank) AS ib_sends
FROM (SELECT DISTINCT rank FROM gpu_access
      UNION SELECT DISTINCT rank FROM ib_post_send) r
ORDER BY CAST(r.rank AS INTEGER);
