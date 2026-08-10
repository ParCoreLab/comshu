-- requires: comm_edges params.ranks
-- No chain link may carry comparable traffic in the reverse direction.
--
-- THE REASON THIS CASE EXISTS. Every other NVSHMEM fixture here is a ring or a
-- pair, so A->B and B->A were always both present in equal volume, and an
-- aggregate that silently symmetrised an edge produced the right answer on all
-- of them. This is the rank-space counterpart of 14_pipeline's device-space
-- check, and it tests the derived layer the UI matrix actually reads.
--
-- COMPARABLE VOLUME, not rank order, and both simpler formulations were tried
-- and rejected against mutated captures:
--
--   top (ranks-1) edges: the window holds exactly the chain, so a heavy reverse
--     edge DISPLACES a forward one rather than appearing beside it. The
--     assertion could never fire — verified silent on a capture carrying a
--     999999-byte back-flow edge.
--   top 2*(ranks-1): now big enough to hold both directions, but it also admits
--     NVSHMEM's bootstrap mesh, which is genuinely bidirectional. Fired on a
--     CLEAN capture.
--
-- So compare each chain link against its own reverse. A symmetrising aggregate
-- yields an exactly equal reverse and fails loudly; the bootstrap mesh is ~2% of
-- a data edge and passes. The 1/4 is a wide margin around "comparable", not a
-- tuned cutoff — nothing legitimate sits between 2% and 100%.
WITH RECURSIVE seq(i) AS (
    SELECT 0 UNION ALL SELECT i+1 FROM seq WHERE i+1 < (SELECT ranks FROM params)
),
chain AS (
    SELECT i AS src, i+1 AS dst FROM seq WHERE i+1 < (SELECT ranks FROM params)
),
vol AS (
    SELECT CAST(src_rank AS INTEGER) src, CAST(dst_rank AS INTEGER) dst,
           sum(CAST(bytes AS INTEGER)) AS b
    FROM comm_edges WHERE transport <> 'nvlink' GROUP BY 1, 2
)
SELECT 'reverse traffic comparable to the forward link on a one-way chain' AS problem,
       c.src, c.dst,
       coalesce(f.b, 0) AS forward_bytes,
       coalesce(r.b, 0) AS reverse_bytes
FROM chain c
LEFT JOIN vol f ON f.src = c.src AND f.dst = c.dst
LEFT JOIN vol r ON r.src = c.dst AND r.dst = c.src
WHERE coalesce(r.b, 0) * 4 >= coalesce(f.b, 0)
ORDER BY c.src;
