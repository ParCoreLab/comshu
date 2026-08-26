-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.natoms params.nsteps params.nstlist
-- The PP->PME byte volume follows from the system size, not from the run.
--
-- ORACLE. Each PP rank sends the coordinates of its HOME atoms to its PME rank
-- once per force evaluation, and the PP ranks' home-atom sets partition the
-- system, so the PP->PME coordinate volume per step is natoms * sizeof(rvec)
-- however the decomposition moved atoms between domains:
--   src/gromacs/ewald/pme_pp.cpp:296   MPI_Isend(x.data(), n*sizeof(rvec), ...)   [staged]
--   pme_pp_comm_gpu_impl_gpu.cpp:256   MPI_Isend(sendptr_x, sendSize*DIM, MPI_FLOAT)  [direct]
-- Both are 12 bytes per atom. Over nsteps+1 force evaluations (step 0 through
-- step nsteps inclusive) that is rvec_bytes * natoms * (nsteps+1).
--
-- Charges are re-sent whenever atoms are redistributed — PP_PME_CHARGE,
-- pme_pp.cpp:212 MPI_Isend(chargeA.data(), ...) — which with -nstlist fixed and
-- -dlb no happens nsteps/nstlist + 1 times, adding real_bytes * natoms each. It
-- is 0.3 % of the total here and is included so the expected value is derived
-- rather than rounded.
--
-- This is the assertion that makes the direct-GPU-comm comparison legible: it
-- is stated once and must hold in BOTH arms. GROMACS moves the same atoms
-- through the same MPI calls either way — what the knob changes is which memory
-- those calls read from. If the volume ever differed between the arms, the
-- reading would be that the tracer lost traffic, not that the knob saved any.
--
-- +-5 % because `count` on ucx_tag_send is the UCP datatype count, which is
-- bytes only for a contiguous datatype (Open MPI's pml_ucx uses
-- ucp_dt_make_contig(1), so it is), and because the exact number of energy /
-- virial exchange messages folded into the same edge is small but not derived
-- here. It is not a fitted tolerance: the two candidate readings this assertion
-- exists to separate — "the volume is natoms-scaled" and "the volume changed
-- with the knob" — differ by far more than 5 %.
WITH RECURSIVE
rk(n) AS (
    SELECT 0
    UNION ALL SELECT n + 1 FROM rk WHERE n + 1 < (SELECT ranks FROM params)
),
pp AS (
    SELECT n AS pp_rank,
           (SELECT npp FROM params)
           + (n * (SELECT npme FROM params) + (SELECT npme FROM params) / 2)
             / (SELECT npp FROM params) AS pme_rank
    FROM rk WHERE n < (SELECT npp FROM params)
),
peer AS (
    SELECT CAST(c.rank AS INTEGER) AS src, CAST(w.rank AS INTEGER) AS dst, c.endpoint AS ep
    FROM ucx_ep_connection c
    JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
),
edge AS (
    SELECT p.src, p.dst, count(*) AS msgs, sum(CAST(s.count AS INTEGER)) AS bytes
    FROM ucx_tag_send s
    JOIN peer p ON p.src = CAST(s.rank AS INTEGER) AND p.ep = s.endpoint
    GROUP BY p.src, p.dst
),
obs AS (
    SELECT coalesce(sum(e.bytes), 0) AS observed
    FROM edge e JOIN pp ON pp.pp_rank = e.src AND pp.pme_rank = e.dst
),
pred AS (
    SELECT (SELECT rvec_bytes FROM params) * (SELECT natoms FROM params)
             * ((SELECT nsteps FROM params) + 1)
         + (SELECT real_bytes FROM params) * (SELECT natoms FROM params)
             * ((SELECT nsteps FROM params) / (SELECT nstlist FROM params) + 1) AS expected
)
SELECT 'PP->PME tagged-send volume is not natoms * rvec_bytes per force evaluation' AS problem,
       obs.observed, pred.expected,
       (100 * obs.observed) / pred.expected AS pct_of_expected
FROM obs, pred
WHERE obs.observed * 100 < pred.expected * 95
   OR obs.observed * 100 > pred.expected * 105;
