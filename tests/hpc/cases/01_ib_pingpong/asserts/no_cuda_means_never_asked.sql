-- requires: mem_object
-- ibv_rc_pingpong never loads CUDA, so no memory object here was ever asked
-- about: every one must say DriverAbsent — not HostNotCuda, which would claim
-- a query that never ran.
--
-- This is the negative test that makes AttrStatus real rather than decorative
-- (PLAN-20260817-0634 step 6). Before it, "device 65535" in a host-only run and
-- in a CUDA run where the query said "host" were the same bytes, and the
-- difference between "we asked" and "we could not ask" was a belief.
SELECT CAST(rank AS INTEGER) AS rank, attr_status, count(*) AS objects
FROM mem_object
WHERE attr_status != 'DriverAbsent'
GROUP BY rank, attr_status;
