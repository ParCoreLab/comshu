-- requires: ucx_rkey entity_close
-- A key may be closed once, and only after it was opened.
--
-- The lifetime is the whole reason UcxRkey is an Entity rather than an
-- Artifact. A ucp_rkey_h is a heap pointer; UCX frees them and the allocator
-- hands the same address back. Without a matched open and close, a key unpacked
-- for a DIFFERENT peer region later in the run is indistinguishable from the
-- first one, and every join in this directory silently starts answering about
-- the wrong window. This workload makes that concrete: five MPI_Win_allocate /
-- MPI_Win_free cycles in one process, one after another, which is precisely the
-- pattern that recycles a handle.
--
-- TWO CLAUSES. The first is bookkeeping. The second is the one that catches a
-- real defect: closes outnumbering opens is what the ENDPOINT doors produced
-- before close_endpoint_once existed — UCX reaches its own public close symbols
-- through one another, all of them interposed, so one teardown fired several
-- hooks. If ucp_rkey_destroy is ever reached through another interposed door
-- the same way, this is what says so, and this workload frees five windows so
-- it would say so five times over.
--
-- 626 is UcxRkey's type id, published in the schema comment and discoverable
-- from `pcomm types`. Scoped to it rather than counting all closes, because
-- entity_close is shared by every provider in the capture.
SELECT 'an entity_close names an rkey that was never opened' AS problem,
       c.rank                                                AS rank,
       c.subject                                             AS rkey
FROM entity_close c
WHERE c.opener = 626
  AND NOT EXISTS (SELECT 1 FROM ucx_rkey k
                  WHERE k.rank = c.rank AND k.rkey = c.subject)
UNION ALL
SELECT 'an rkey was closed more times than it was opened', k.rank, k.rkey
FROM ucx_rkey k
WHERE (SELECT count(*) FROM entity_close c
       WHERE c.opener = 626 AND c.rank = k.rank AND c.subject = k.rkey)
      > (SELECT count(*) FROM ucx_rkey k2
         WHERE k2.rank = k.rank AND k2.rkey = k.rkey);
