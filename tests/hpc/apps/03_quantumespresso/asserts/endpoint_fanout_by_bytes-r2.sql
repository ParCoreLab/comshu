-- requires: ucx_tag_send ucx_endpoint params.nproc_bgrp
-- Round 1's surviving claim, re-run at a different scale. Each rank's heavy
-- tagged-send partners must number exactly nproc_bgrp - 1.
--
-- The FFT transpose is an all-to-all over intra_bgrp_comm, so every rank
-- exchanges the bulk of its bytes with the other nproc_bgrp - 1 members of its
-- band group and with nobody else. nproc_bgrp = ranks/(npool*nbgrp) is fixed by
-- Modules/mp_pools.f90:73 and Modules/mp_bands.f90:90 and is declared in
-- meta-r2.json: 8 at npool=1, 4 at npool=2, so the predicted heavy fan-out is
-- 7 and 3 respectively.
--
-- BYTES, NOT MESSAGES, AND ROUND 1 IS WHY. Its count-based sibling
-- (endpoint_fanout_matches_pool_size.sql) is marked expect_fail because this
-- application falsifies the proxy: at npool=2 rank 0's second endpoint carried
-- 2 638 sends whose commonest lengths were 0, 4, 8 and 1 024 B against 7 340
-- sends of 4 921 600 B on the first -- control traffic, heavy in count and
-- negligible in bytes. The independent reason to prefer bytes predates that
-- capture: 19_ucx_mpi's workload_note refuses to assert UCP counts at all.
--
-- THE THRESHOLD IS DERIVED, NOT CHOSEN. "Heavy" is at least HALF the equal
-- share, 0.5/(nproc_bgrp - 1) of the rank's tagged-send bytes, written as the
-- multiplication `2 * bytes * (nproc_bgrp - 1) >= total` so it needs no division
-- and means the same on either SQL engine. Under a uniform all-to-all exactly
-- nproc_bgrp - 1 endpoints clear it, at either arm's fan-out, with no per-arm
-- constant anywhere.
--
-- STATED OVER ENDPOINTS, NEVER PEER RANKS, and that is a measured constraint
-- rather than a preference: round 1 established that ucx_worker_address is never
-- written for an OpenMPI point-to-point application, because mca_pml_ucx.so
-- publishes its worker address through ucp_worker_query while pcomm hooks
-- ucp_worker_get_address. The only cross-rank UCX key is therefore empty and
-- the peer of an endpoint is not recoverable. Every claim in this study that
-- could have been about ranks is about endpoints for this reason.
--
-- ucx_tag_send.count is a UCP datatype count; MPI_DOUBLE_COMPLEX is contiguous,
-- so it is proportional to bytes and the SHARE is unaffected by the constant.
-- Nothing here compares it to a byte figure.
--
-- `count` is a reserved-ish word in some engines and is a column here; it is
-- always written qualified.
WITH per_ep AS (
  SELECT s.rank                                AS rank,
         s.endpoint                            AS endpoint,
         sum(CAST(s.count AS INTEGER))         AS units
  FROM ucx_tag_send s
  GROUP BY s.rank, s.endpoint
),
per_rank AS (
  SELECT rank, sum(units) AS total FROM per_ep GROUP BY rank
),
heavy AS (
  SELECT p.rank, count(*) AS n_heavy
  FROM per_ep p JOIN per_rank r ON r.rank = p.rank
  WHERE r.total > 0
    AND p.units * 2 * ((SELECT nproc_bgrp FROM params) - 1) >= r.total
  GROUP BY p.rank
)
SELECT 'a rank''s heavy tagged-send fan-out is not nproc_bgrp - 1' AS problem,
       h.rank,
       h.n_heavy                                   AS measured,
       (SELECT nproc_bgrp - 1 FROM params)         AS predicted
FROM heavy h
WHERE h.n_heavy <> (SELECT nproc_bgrp - 1 FROM params)

UNION ALL

SELECT 'a rank posted tagged sends but produced no fan-out row at all',
       r.rank, 0, (SELECT nproc_bgrp - 1 FROM params)
FROM per_rank r
WHERE r.rank NOT IN (SELECT rank FROM heavy);
