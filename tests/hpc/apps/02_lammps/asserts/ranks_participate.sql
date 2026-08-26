-- requires: ucx_tag_send params.ranks
-- Every rank the job launched must have sent through UCX.
-- LAMMPS gives every rank a subdomain with six faces and no rank is idle:
-- CommBrick::forward_comm (src/comm_brick.cpp:497) runs the same nswap loop on
-- every rank, and src/comm.cpp:351's nswap is a global (Allreduce'd) quantity.
-- Offending rows = ranks that should have sent and did not, or that should not
-- exist and did.
SELECT r.rank AS missing_or_extra_rank
FROM (SELECT DISTINCT rank FROM ucx_tag_send) r
WHERE r.rank < 0 OR r.rank >= (SELECT ranks FROM params)
UNION ALL
SELECT p.n
FROM (SELECT 0 AS n UNION SELECT 1 UNION SELECT 2 UNION SELECT 3
      UNION SELECT 4 UNION SELECT 5 UNION SELECT 6 UNION SELECT 7) p
WHERE p.n < (SELECT ranks FROM params)
  AND p.n NOT IN (SELECT DISTINCT rank FROM ucx_tag_send);
