-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.npp params.npme
-- Every PP rank talks to the PME rank the domain decomposition assigns it.
--
-- ORACLE (GROMACS source, not the capture):
--   src/gromacs/domdec/domdec.cpp:540  ddindex2pmeindex(i) = (i*npme + npme/2)/npp
--   src/gromacs/domdec/domdec.cpp:623  dd_simnode2pmenode(), pmeRanks.empty()
--                                      branch = numPPRanks + ddindex2pmeindex(sim_nodeid)
-- and every PME message a PP rank sends is addressed to that one rank:
--   src/gromacs/ewald/pme_pp.cpp:184,197,212,296  MPI_Isend(..., dd->pme_nodeid, ...)
--   src/gromacs/ewald/pme_pp_comm_gpu_impl_gpu.cpp:256  MPI_Isend(..., pmeRank_, ...)
-- With -ddorder pp_pme the formula is evaluated here in SQL from npp and npme,
-- so this file names no rank the run produced — change -npme in meta.json and
-- the expected fan-in changes with it.
--
-- The rank graph is reconstructed the only way the UCX records allow: an
-- endpoint carries the hash of the remote worker address it was dialled at
-- (ucx_ep_connection.remote_prefix_hash), and the worker on the other side
-- hashed its own address the same way (ucx_worker_address.prefix_hash). That
-- join is the one thing in the UCX family that crosses a rank boundary.
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
)
SELECT 'PP rank sent nothing to the PME rank its DD index assigns it' AS problem,
       pp.pp_rank, pp.pme_rank,
       coalesce(e.msgs, 0)  AS msgs,
       coalesce(e.bytes, 0) AS bytes
FROM pp LEFT JOIN edge e ON e.src = pp.pp_rank AND e.dst = pp.pme_rank
WHERE coalesce(e.msgs, 0) = 0
ORDER BY pp.pp_rank;
