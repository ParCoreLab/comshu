-- requires: ucx_rkey entity_close
-- A key may be closed once, and only after it was opened.
--
-- The lifetime is the whole reason UcxRkey is an Entity rather than an Artifact.
-- A `ucp_rkey_h` is a heap pointer and UCX frees and reallocates them, so
-- without an open and a close the same address unpacked for a DIFFERENT peer
-- region is indistinguishable from the first — range-containment identity is
-- sound only within a lifetime, which records.hpp says outright.
--
-- TWO CLAUSES, and the second is the one that catches a real defect rather than
-- a bookkeeping slip. Closes outnumbering opens is what the endpoint doors
-- produced before `close_endpoint_once` existed: UCX reaches its own public
-- close symbols through one another, all of them interposed, so one teardown
-- fired several hooks and endpoint 4 on rank 0 closed THREE times inside 1.9 us
-- on job 44698048. If ucp_rkey_destroy is ever reached through another
-- interposed door the same way, this is what says so.
--
-- Opener 626 is UcxRkey's type id. Scoped to it rather than counting all closes,
-- because entity_close is shared by every provider in the capture and a global
-- count would be answering a different question.
SELECT 'an entity_close names an rkey that was never opened' AS problem,
       c.subject                                             AS unopened_rkey
FROM entity_close c
WHERE c.opener = 626
  AND NOT EXISTS (SELECT 1 FROM ucx_rkey k
                  WHERE k.rank = c.rank AND k.rkey = c.subject)
UNION ALL
SELECT 'an rkey was closed more than once', k.rkey
FROM ucx_rkey k
WHERE (SELECT count(*) FROM entity_close c
       WHERE c.opener = 626 AND c.rank = k.rank AND c.subject = k.rkey)
      > (SELECT count(*) FROM ucx_rkey k2
         WHERE k2.rank = k.rank AND k2.rkey = k.rkey);
