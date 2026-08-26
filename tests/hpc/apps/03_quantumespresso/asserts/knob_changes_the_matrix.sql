-- requires: two-captures ucx_tag_send params.ranks
-- THE HEADLINE. One flag changed; the traffic must concentrate, and by how much
-- is fixed by the flag's own arithmetic.
--
-- Both captures are the same binary, the same input, the same 4 ranks on the
-- same 4 nodes, the same MPI and UCX settings and the same transport. `-npool`
-- is the only difference.
--
-- THE MEASURE, chosen to need no threshold and no peer-rank mapping: for each
-- rank, the share of its tagged sends carried by its BUSIEST ucp endpoint. Then
-- take the smallest such share across ranks, so one chatty rank cannot carry the
-- claim.
--
-- THE PREDICTION, from Modules/mp_pools.f90:73 (nproc_pool = ranks / npool) and
-- FFTXlib/src/fft_scatter.f90:715 (the all-to-all runs on a communicator inside
-- the pool):
--   npool=1 : nproc_pool = ranks, so the transpose spreads each rank's traffic
--             over ranks-1 peers and the busiest share is about 1/(ranks-1).
--   npool=2 : nproc_pool = ranks/2, so the same traffic collapses onto
--             ranks/2 - 1 peers and the busiest share rises.
-- Therefore min-over-ranks(busiest share) must be strictly GREATER in the
-- npool=2 capture than in the npool=1 capture.
--
-- Stated as an inequality, not as two numbers: the shares depend on how many
-- Davidson steps ran, which is data-dependent, but their ORDER is fixed by the
-- decomposition. A capture reporting the same concentration for both would mean
-- the knob is invisible in the trace — the failure this exercise exists to be
-- able to detect.
WITH total AS (
    SELECT cfg, CAST(rank AS INTEGER) AS rank, count(*) AS n
    FROM ucx_tag_send GROUP BY 1, 2
),
per_ep AS (
    SELECT cfg, CAST(rank AS INTEGER) AS rank, endpoint, count(*) AS n
    FROM ucx_tag_send GROUP BY 1, 2, 3
),
share AS (
    SELECT t.cfg, t.rank, max(1.0 * e.n / t.n) AS busiest
    FROM total t JOIN per_ep e ON e.cfg = t.cfg AND e.rank = t.rank
    GROUP BY t.cfg, t.rank
),
worst AS (SELECT cfg, min(busiest) AS busiest FROM share GROUP BY cfg)
SELECT 'npool did not concentrate each rank''s traffic onto fewer endpoints' AS problem,
       (SELECT busiest FROM worst WHERE cfg = 'a') AS npool1_busiest_share,
       (SELECT busiest FROM worst WHERE cfg = 'b') AS npool2_busiest_share
WHERE NOT ((SELECT busiest FROM worst WHERE cfg = 'b')
         > (SELECT busiest FROM worst WHERE cfg = 'a'));
