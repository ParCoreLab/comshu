-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.npp params.npme
-- The other half of the PME asymmetry: the fan-in has a fan-out.
--
-- A PP rank sends coordinates to its PME rank and the PME rank sends the mesh
-- forces back to it — src/gromacs/ewald/pme_pp.cpp:566 MPI_Recv(recvptr, ...,
-- cr->dd->pme_nodeid, ...) on the staged path, and
-- pme_pp_comm_gpu_impl_gpu.cpp:186/196/202 MPI_Recv(..., pmeRank_, ...) on the
-- direct path. Asserting only the fan-in would pass on a capture in which the
-- PME rank was a sink, which is not a shape GROMACS can produce.
--
-- This is the assertion that makes the topology genuinely ASYMMETRIC rather than
-- merely sparse: one rank out of `ranks` has an edge to every other, and the
-- rest have edges to three peers each (two DD neighbours plus PME). Both facts
-- come from the command line -- see halo_partners.sql for the other one.
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
SELECT 'PME rank sent nothing back to a PP rank it serves' AS problem,
       pp.pme_rank AS pme_rank, pp.pp_rank AS pp_rank,
       coalesce(e.msgs, 0)  AS msgs,
       coalesce(e.bytes, 0) AS bytes
FROM pp LEFT JOIN edge e ON e.src = pp.pme_rank AND e.dst = pp.pp_rank
WHERE coalesce(e.msgs, 0) = 0
ORDER BY pp.pp_rank;
