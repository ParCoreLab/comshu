-- requires: ucx_tag_send ucx_ep_connection ucx_worker_address params.npp params.dd_y params.dd_z
-- A PP rank's bulk traffic goes to its two DD-grid neighbours and its PME rank,
-- and nowhere else.
--
-- ORACLE. npp = 7 is prime, so nx*ny*nz = npp admits only a permutation of
-- 7x1x1 and -dd 7 1 1 states it on the command line. A 1-D periodic DD grid of
-- npp cells gives cell x exactly two neighbours, (x+1) mod npp and (x-1) mod
-- npp. GROMACS stages halo exchange one DIMENSION at a time — a rank's direct
-- MPI partners are its neighbours along each dimension, and diagonal zones are
-- reached by forwarding already-received data, never by a message to the
-- diagonal rank:
--   src/gromacs/domdec/gpuhaloexchange_impl_gpu.cpp:489-492 (direct path)
--   src/gromacs/domdec/haloexchange.cpp:89,126 (staged path)
-- With one dimension there is nothing to forward, so the partner set is exactly
-- {x-1, x+1} plus the PME rank. Every rank in this query is computed from npp
-- and npme; none is read off the capture.
--
-- WHY A THRESHOLD AND NOT "no edge at all". GROMACS reduces energies and
-- virials over all PP ranks (global_stat), and Open MPI implements those with
-- recursive-doubling algorithms whose partners are r XOR 2^k — so a handful of
-- small messages between non-neighbours is CORRECT and expected. The claim is
-- about where the bulk goes: halo exchange moves natoms-scale coordinate and
-- force buffers every step, a global reduction moves tens of bytes every
-- nstcalcenergy steps. 5 % of a rank's outbound bytes separates them by three
-- orders of magnitude; it is not a tuned number and nothing here was fitted to
-- an observation.
--
-- The dd_y/dd_z requirement is a guard, not decoration: this file is only a
-- correct specification for a 1-D decomposition, and `-- requires:` makes it
-- skip rather than lie if the case is ever re-run with a 2-D grid.
WITH
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
tot AS (SELECT src, sum(bytes) AS total FROM edge GROUP BY src)
SELECT 'PP rank sends bulk traffic to a rank that is neither a DD neighbour nor its PME rank' AS problem,
       e.src, e.dst, e.msgs, e.bytes, t.total,
       (100 * e.bytes) / t.total AS pct_of_rank_bytes
FROM edge e JOIN tot t ON t.src = e.src
WHERE e.src < (SELECT npp FROM params)
  AND e.dst <> e.src
  AND (SELECT dd_y FROM params) = 1
  AND (SELECT dd_z FROM params) = 1
  AND e.bytes * 20 > t.total
  AND e.dst <> ((SELECT npp FROM params)
                + (e.src * (SELECT npme FROM params) + (SELECT npme FROM params) / 2)
                  / (SELECT npp FROM params))
  AND e.dst <> ((e.src + 1) % (SELECT npp FROM params))
  AND e.dst <> ((e.src - 1 + (SELECT npp FROM params)) % (SELECT npp FROM params))
ORDER BY e.src, e.dst;
