-- requires: ucx_rma ucx_rkey
-- Included KNOWING it should pass vacuously, and that is the claim.
--
-- QE's parallel layer (UtilXlib/mp.f90) is two-sided: the plane-wave DFT
-- primitives are alltoall, allreduce, bcast and gather. So ucx_rma is PREDICTED
-- EMPTY for this application and this assertion is expected to return no rows
-- because there is nothing to check, not because everything checked out.
--
-- IT IS HERE FOR TWO REASONS.
--
--  1. If it ever fires, QE reached a one-sided door and the study's whole
--     premise about which primitives matter is wrong. An assertion that costs
--     one scan and would catch that is cheap insurance.
--
--  2. It records, in the place a reader will look, that the sibling gap "no
--     documented join from a one-sided operation to the peer rank" IS NOT
--     TESTABLE ON QUANTUM ESPRESSO. That gap belongs to a one-sided workload
--     (05_osu_onesided). What CAN be said from the documentation alone, without
--     a capture: the join is documented but nothing performs it. ucx_rma names
--     an endpoint (comshu.fbs 610); ucx_ep_connection maps an endpoint to a
--     remote_prefix_hash (624); demos/figures/11-cross-rank-keys.svg names
--     prefix_hash as one of the five cross-rank keys, joining to another rank's
--     ucx_worker_address (622). So the chain exists on paper. What does not
--     exist is any DERIVED table documented to walk it for a one-sided op --
--     `endpoint` is documented as "one row per local queue pair", which is
--     verbs, and nothing states whether RMA contributes rows to comm_edges. And
--     on the OpenMPI point-to-point path the key is empty anyway, for round 1's
--     measured reason. See QUESTIONS.md.
--
-- The claim itself is 22_ucx_rma's: an RMA row that names a key nothing created
-- says a transfer happened and cannot say which peer region it targeted --
-- remote_addr alone is a bare virtual address with no owner. Scoped to keys
-- actually named by a recorded operation rather than asserting a count, because
-- how many keys MPI unpacks for one window is MPI's business.
SELECT 'an RMA row names an rkey that no ucx_rkey row created' AS problem,
       r.rkey    AS orphan_rkey,
       count(*)  AS rma_rows
FROM ucx_rma r
WHERE r.rkey <> 0
  AND NOT EXISTS (SELECT 1 FROM ucx_rkey k
                  WHERE k.rank = r.rank AND k.rkey = r.rkey)
GROUP BY r.rkey;
