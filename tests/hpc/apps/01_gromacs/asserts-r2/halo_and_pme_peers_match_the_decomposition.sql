-- requires: ucx_tag_recv params.npp params.ompi_rank_shift params.ompi_rank_mask
-- The peers a PP rank receives the most bytes from must be its two domain
-- neighbours and the PME rank -- and nothing else.
--
-- THE ORACLE. GROMACS's decomposition, twice over:
--   * domdec/domdec.cpp:1062 setup_neighbor_relations gives DD cell x the
--     neighbours (x +- 1) mod 7 at one communication pulse, and GROMACS 2025
--     exchanges the halo directly with each (domdec/haloexchange.cpp:126,
--     MPI_Irecv(..., receive.rank(), ...)). Rank x receives coordinates from one
--     and forces from the other, so both appear.
--   * domdec/domdec.cpp:540,552 put the single PME rank at world rank 7 and
--     every PP rank receives its forces from it (ewald/pme_pp.cpp:566).
-- Three peers, and which three follows from the rank number alone. The PME rank
-- itself receives coordinates from all seven PP ranks.
--
-- THE PEER COMES FROM THE TAG, NOT FROM A CROSS-RANK JOIN. Open MPI's
-- PML_UCX_MAKE_RECV_TAG (pml_ucx_request.h:48) writes the requested source into
-- bits 20-39 of the UCP tag, and PML_UCX_TAG_GET_SOURCE(:65) reads it back with
-- exactly the shift and mask below. An MPI_ANY_SOURCE receive writes
-- ((uint64_t)-1 & UCS_MASK(20)) = 0xFFFFF into that field, which is 1048575 and
-- not a valid rank, so it excludes itself from this query without needing the
-- tag_mask at all. sender_rank_is_encoded_in_its_own_tags.sql is what certifies
-- that the decode works before this file relies on it.
--
-- WHY THIS IS THE CLAIM WORTH MAKING. Round 1 concluded that no rank-to-rank
-- topology is recoverable for an HPC-X application, because the one documented
-- cross-rank key (ucx_ep_connection -> ucx_worker_address.prefix_hash) resolves
-- the wrong worker on Open MPI 4.1.x. That finding stands and the fix named in
-- F2 is still worth making. But the conclusion drawn from it was too strong:
-- the peer was in the capture the whole time, in a field the schema documents
-- as an opaque 64-bit tag. This assertion is the proof, against a partner set
-- the application's source determines and the tool has never seen.
--
-- RANKED BY VOLUME, NOT ASSERTED AS A SET, and the reason is Open MPI rather
-- than GROMACS. The tuned collective module pairs ranks by recursive doubling,
-- so a global reduction gives rank r low-volume receives from r^1, r^2, r^4 --
-- real traffic, correctly recorded, and no part of the decomposition. Halo and
-- PME traffic is hundreds of megabytes per rank; a reduction of a few dozen
-- reals at every hundredth step is kilobytes. So the claim is about the top of
-- the ranking, where the difference is five orders of magnitude, and it stays
-- true whatever collective algorithm Open MPI picks.
--
-- `count` is the capacity OFFERED by the receive, not bytes delivered -- the
-- record comment says so and this file does not treat it as a byte total. It is
-- used only to ORDER peers, and GROMACS sizes its receives exactly (it exchanges
-- atom counts before the halo), so the ordering is sound even where the
-- magnitude would not be.
WITH RECURSIVE r(rk) AS (
  SELECT 0
  UNION ALL
  SELECT rk + 1 FROM r WHERE rk + 1 < (SELECT ranks FROM params)
),
expected AS (
  SELECT rk AS a, (rk + 1) % (SELECT npp FROM params) AS b
  FROM r WHERE rk < (SELECT npp FROM params)
  UNION
  SELECT rk, (rk + (SELECT npp FROM params) - 1) % (SELECT npp FROM params)
  FROM r WHERE rk < (SELECT npp FROM params)
  UNION
  SELECT rk, (SELECT npp FROM params)
  FROM r WHERE rk < (SELECT npp FROM params)
  UNION
  SELECT (SELECT npp FROM params), rk
  FROM r WHERE rk < (SELECT npp FROM params)
),
-- How many peers each rank's decomposition gives it: 3 for a PP rank, npp for
-- the PME rank. That is how deep into the volume ranking this looks.
depth AS (
  SELECT a AS rk, count(*) AS n FROM expected GROUP BY a
),
obs AS (
  SELECT CAST(rank AS INTEGER) AS rk,
         ((CAST(tag AS BIGINT) >> (SELECT ompi_rank_shift FROM params))
          & (SELECT ompi_rank_mask FROM params)) AS peer,
         sum(CAST("count" AS BIGINT)) AS offered
  FROM ucx_tag_recv
  WHERE ((CAST(tag AS BIGINT) >> (SELECT ompi_rank_shift FROM params))
         & (SELECT ompi_rank_mask FROM params)) < (SELECT ranks FROM params)
  GROUP BY CAST(rank AS INTEGER),
           ((CAST(tag AS BIGINT) >> (SELECT ompi_rank_shift FROM params))
            & (SELECT ompi_rank_mask FROM params))
),
ranked AS (
  SELECT rk, peer, offered,
         row_number() OVER (PARTITION BY rk ORDER BY offered DESC, peer) AS pos
  FROM obs
)
SELECT 'a rank''s highest-volume receive peers are not the ones its decomposition names' AS problem,
       k.rk AS rank, k.peer, k.offered, k.pos
FROM ranked k JOIN depth d ON d.rk = k.rk
WHERE k.pos <= d.n
  AND NOT EXISTS (SELECT 1 FROM expected e WHERE e.a = k.rk AND e.b = k.peer)

UNION ALL

-- The converse, and it fails differently: a partner the decomposition requires
-- that does not appear in the top of the ranking at all. Catches a lost edge
-- rather than a spurious one.
SELECT 'a partner the decomposition requires is not among that rank''s top peers',
       e.a, e.b, -1, -1
FROM expected e JOIN depth d ON d.rk = e.a
WHERE EXISTS (SELECT 1 FROM ranked k WHERE k.rk = e.a)
  AND NOT EXISTS (SELECT 1 FROM ranked k
                  WHERE k.rk = e.a AND k.peer = e.b AND k.pos <= d.n)

UNION ALL

-- Decode canary, for the same reason as in the sender-rank file: without it a
-- CSV export whose tag column is a hex string makes `obs` empty and this whole
-- assertion pass while testing nothing.
SELECT 'no receive tag decoded to a valid peer rank - the tag column is not an integer here',
       -1, -1, (SELECT count(*) FROM ucx_tag_recv), -1
WHERE (SELECT count(*) FROM ucx_tag_recv) > 0
  AND NOT EXISTS (SELECT 1 FROM obs);
