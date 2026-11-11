-- requires: ucx_tag_send params.npp params.natoms params.nsteps params.rvec_bytes
-- The PME ranks' outbound volume follows from the system size alone.
--
-- SAME ORACLE as pme_coord_volume.sql, asked in a way that does not depend on
-- comshu being able to name the peer. A PME-only rank does exactly one thing with
-- MPI: it receives coordinates from the PP ranks it serves and sends the mesh
-- forces back (src/gromacs/ewald/pme_only.cpp, gmx_pme_send_force_vir_ener ->
-- pme_pp.cpp). Its home-atom sets partition the system, so its outbound volume
-- per force evaluation is natoms * sizeof(rvec) = 12 B/atom, and over nsteps+1
-- evaluations (step 0 through step nsteps) that is
--
--     rvec_bytes * natoms * (nsteps + 1)
--
-- plus one small energy/virial struct per energy step, which is why the band is
-- not exactly zero-width.
--
-- WHY THIS FILE EXISTS BESIDE pme_coord_volume.sql. It is not a weakened
-- restatement to make a red assertion green — pme_coord_volume.sql stays
-- exactly as written and is marked expect_fail with its evidence. The two ask
-- the same question of different parts of the capture:
--
--   pme_coord_volume   needs the PP -> PME EDGE, which requires the cross-rank
--                      join ucx_ep_connection.remote_prefix_hash ->
--                      ucx_worker_address.prefix_hash. On this stack that join
--                      is broken (see meta.json expect_fail_asserts) and the
--                      assertion cannot be evaluated, which is a fact about the
--                      TOOL.
--   this file          needs only `rank` and `count` on ucx_tag_send, both of
--                      which are recorded correctly, so it is a statement about
--                      GROMACS that this capture can actually answer.
--
-- Identifying the PME ranks as `rank >= npp` is the command line's own split,
-- not an identifier read off the capture: -npme 1 with -ddorder pp_pme puts PP
-- first and PME last (domdec.cpp:1365).
--
-- 1 % band. The predicted 25,662,580,944 B and the energy/virial remainder are
-- three orders of magnitude apart, so this does not need slack to pass and does
-- not get slack to hide a real difference.
WITH obs AS (
    SELECT coalesce(sum(CAST(s."count" AS INTEGER)), 0) AS observed
    FROM ucx_tag_send s
    WHERE CAST(s.rank AS INTEGER) >= (SELECT npp FROM params)
),
pred AS (
    SELECT (SELECT rvec_bytes FROM params) * (SELECT natoms FROM params)
             * ((SELECT nsteps FROM params) + 1) AS expected
)
SELECT 'PME-rank outbound volume is not natoms * rvec_bytes per force evaluation' AS problem,
       obs.observed, pred.expected,
       obs.observed - pred.expected AS excess_bytes
FROM obs, pred
WHERE obs.observed * 100 < pred.expected * 99
   OR obs.observed * 100 > pred.expected * 101;
