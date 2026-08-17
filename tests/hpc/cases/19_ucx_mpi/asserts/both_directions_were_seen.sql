-- requires: ucx_tag_send ucx_tag_recv
-- The workload is a symmetric ping-pong, so BOTH doors must have fired.
--
-- Every rank sends and receives: rank 0 does Send-then-Recv, rank 1 does
-- Recv-then-Send, 100 times, and then both join an Allreduce. So a capture in
-- which tagged sends exist and tagged receives do not (or the reverse) is not a
-- quiet workload — it is a provider that hooked one side of the pair. That is a
-- realistic failure: send and receive are different UCP doors with different
-- signatures, and the receive side landed in a later commit than the send side.
--
-- NO COUNTS, DELIBERATELY. MPI_Send is not obliged to be one ucp_tag_send —
-- OpenMPI picks eager or rendezvous by size and its collectives take their own
-- path — so asserting 100 would be asserting OpenMPI's protocol choices, not
-- pcomm's recording, and it would go red on an HPC-X upgrade that broke nothing.
-- Presence per rank is the strongest claim that is actually about us.
SELECT 'a rank posted tagged sends but no tagged receives' AS problem, rank
FROM (SELECT DISTINCT rank FROM ucx_tag_send)
WHERE rank NOT IN (SELECT DISTINCT rank FROM ucx_tag_recv)
UNION ALL
SELECT 'a rank posted tagged receives but no tagged sends', rank
FROM (SELECT DISTINCT rank FROM ucx_tag_recv)
WHERE rank NOT IN (SELECT DISTINCT rank FROM ucx_tag_send);
