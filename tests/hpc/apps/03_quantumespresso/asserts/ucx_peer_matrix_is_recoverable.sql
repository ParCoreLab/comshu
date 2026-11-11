-- requires: ucx_tag_send ucx_ep_connection params.ranks
-- The UCX records must be able to name WHICH PEER a tagged send went to.
--
-- THIS ASSERTION IS EXPECTED TO FAIL, and it is written to state the claim
-- rather than to describe what comshu emits (suite/README.md: never edit an
-- assertion to match what we emit — mark it, with evidence, and let the fix flip
-- it). See meta.json's expect_fail_asserts and REPORT.md finding 2.
--
-- THE CLAIM. ucx_records.hpp:275-279 says of UcxEpConnection that joining
-- remote_prefix_hash to ANOTHER rank's UcxWorkerAddress.prefix_hash "is what
-- turns a pile of per-rank endpoint rows into a communication graph, and it is
-- the only thing in the UCX records that crosses a rank boundary at all." So the
-- UCX half of a `-p ib,ucx` capture either resolves its endpoints to ranks or it
-- has no rank-space content whatsoever.
--
-- WHY IT FAILS, named to a symbol. UcxWorkerAddress is produced from exactly one
-- door, ucp_worker_get_address (providers/ucx/ucx_hooks.hpp:181,
-- ucx_provider.cpp:345). OpenMPI's point-to-point layer does not call it. Its
-- undefined symbols (HPC-X 2.16, login-node sweep):
--     mca_pml_ucx.so   -> ucp_worker_query, ucp_worker_release_address
--                         (and NOT ucp_worker_get_address)
--     mca_osc_ucx.so   -> ucp_worker_get_address AND ucp_worker_query
--     mca_spml_ucx.so  -> ucp_worker_get_address AND ucp_worker_query
-- pml_ucx publishes its address through ucp_worker_query with
-- UCP_WORKER_ATTR_FIELD_ADDRESS. The hooked door is the one-sided/OpenSHMEM
-- door, not the door every MPI_Send and MPI_Alltoall goes through. So
-- ucx_worker_address is EMPTY for any OpenMPI p2p application, and every
-- remote_prefix_hash in the capture points at nothing.
--
-- This is the same shape of defect as case 08's: an API hooked in one of its two
-- spellings (ibv_reg_mr falling through to ibv_reg_mr_iova2, ibv_wr_* never
-- touching ctx->ops.post_send), green everywhere until a real application picks
-- the other spelling.
--
-- Names no rank and no handle. The first branch is the defect; the second would
-- catch a 32-byte prefix collision (kAddressHashPrefix, ucx_provider.cpp:129) if
-- the join ever closes, which ucx_records.hpp:262-266 says the two-hash scheme
-- exists to make visible rather than silently wrong.
SELECT 'endpoints were dialled but no worker address exists to resolve them to a rank'
         AS problem,
       (SELECT count(*) FROM ucx_ep_connection)                     AS dialled,
       (SELECT count(*) FROM ucx_tag_send)                          AS sends_thus_unattributed
WHERE NOT EXISTS (SELECT 1 FROM ucx_worker_address)
UNION ALL
SELECT 'a dialled address prefix resolves to more than one rank',
       CAST(c.rank AS INTEGER), count(DISTINCT CAST(w.rank AS INTEGER))
FROM ucx_ep_connection c
JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
GROUP BY c.rank, c.endpoint
HAVING count(DISTINCT CAST(w.rank AS INTEGER)) > 1;
