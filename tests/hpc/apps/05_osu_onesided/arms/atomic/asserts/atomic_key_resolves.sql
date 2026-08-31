-- requires: ucx_atomic ucx_rkey
-- An atomic names a remote key too, and it must resolve.
--
-- Same claim as every_rma_key_resolves in the parent directory, one door over,
-- and it needs its own file because it is reached by a different code path in
-- both layers: ucx_rma.rkey is filled by the put/get doors and ucx_atomic.rkey
-- by the three atomic doors, which are separate hooks over separate UCX
-- symbols with separate argument orders. Passing on one says nothing about the
-- other, and this workload is the only one in the study that produces atomic
-- rows at all.
--
-- WHY AN UNRESOLVED KEY IS WORSE HERE THAN FOR A PUT. An atomic's whole
-- semantics are about a specific remote WORD: the value, the operand width and
-- the remote address are meaningless without knowing whose memory the address
-- is in, and the rkey is the only field that can say. A put at least moves a
-- known payload; a fetch-and-add whose target cannot be identified records that
-- something somewhere was incremented.
--
-- Scoped to keys actually named by a recorded operation, not to a count of
-- keys, for the same reason as its sibling: how many keys OpenMPI unpacks for
-- one window is OpenMPI's business.
SELECT 'an atomic row names an rkey that no ucx_rkey row created' AS problem,
       a.rank                                                     AS rank,
       a.rkey                                                     AS orphan_rkey,
       count(*)                                                   AS atomic_rows
FROM ucx_atomic a
WHERE a.rkey <> 0
  AND NOT EXISTS (SELECT 1 FROM ucx_rkey k
                  WHERE k.rank = a.rank AND k.rkey = a.rkey)
GROUP BY a.rank, a.rkey;
