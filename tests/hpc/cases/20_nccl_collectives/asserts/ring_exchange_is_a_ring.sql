-- requires: nccl_p2p params.ranks
-- The point-to-point edges form the ring the workload wrote.
--
-- This is the assertion that justifies the nccl_p2p record existing at all.
-- ncclSend/ncclRecv are the ONLY NCCL calls that name a peer, so they are the
-- only ones from which an exact edge follows without modelling how a
-- collective decomposes internally — and case 05 is the standing evidence that
-- the other route does not work: 1,179,948 GPU accesses there attribute to no
-- object, because NCCL's traffic runs through buffers the application never
-- allocated.
--
-- Derived from the workload, not observed: nccl_collectives.cu sends to
-- (rank+1) % size and receives from (rank-1+size) % size. Every rank must send
-- to exactly one peer and receive from exactly one, and the successor relation
-- must hold for all of them. `ranks` is the only constant, and it comes from
-- params.
--
-- A capture that lost a rank fails the first clause. A dir field decoded
-- backwards fails the third and fourth together, which is the failure mode a
-- golden would have shown as two swapped numbers nobody would question.
SELECT * FROM (
  SELECT 'not every rank recorded point-to-point' AS problem, NULL AS rank, NULL AS peer
  WHERE (SELECT count(DISTINCT rank) FROM nccl_p2p) != (SELECT ranks FROM params)

  UNION ALL
  SELECT 'rank sends to more than one peer', CAST(rank AS INTEGER), count(DISTINCT peer)
  FROM nccl_p2p WHERE dir = 'send' GROUP BY rank HAVING count(DISTINCT peer) != 1

  UNION ALL
  SELECT 'rank receives from more than one peer', CAST(rank AS INTEGER), count(DISTINCT peer)
  FROM nccl_p2p WHERE dir = 'recv' GROUP BY rank HAVING count(DISTINCT peer) != 1

  UNION ALL
  -- The ring closes forwards: rank r sends to r+1 (mod ranks).
  SELECT 'send peer is not the ring successor', CAST(rank AS INTEGER), CAST(peer AS INTEGER)
  FROM nccl_p2p WHERE dir = 'send'
    AND peer != (rank + 1) % (SELECT ranks FROM params)

  UNION ALL
  -- And backwards: rank r receives from r-1 (mod ranks).
  SELECT 'recv peer is not the ring predecessor', CAST(rank AS INTEGER), CAST(peer AS INTEGER)
  FROM nccl_p2p WHERE dir = 'recv'
    AND peer != (rank - 1 + (SELECT ranks FROM params)) % (SELECT ranks FROM params)

  -- Deliberately NOT asserted: that a send on rank r has a matching recv on
  -- rank r+1 at a comparable time. These are enqueue records on independent
  -- host clocks, and a cross-rank time comparison needs the clock anchor. The
  -- structural pairing above is what this capture can honestly claim.
)
ORDER BY problem, rank;
