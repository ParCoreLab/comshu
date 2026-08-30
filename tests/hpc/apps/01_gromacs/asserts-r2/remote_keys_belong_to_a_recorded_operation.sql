-- requires: ucx_rkey ucx_rma
-- Every remote key in the capture must belong to an operation the capture
-- recorded.
--
-- GROMACS ISSUES NO ONE-SIDED MPI. It creates no window and calls neither
-- MPI_Put nor MPI_Get; its communication is MPI_Isend/MPI_Irecv for the halo and
-- PP<->PME (domdec/haloexchange.cpp:126, ewald/pme_pp.cpp:296) plus collectives.
-- So `ucx_rma` and `ucx_atomic` are predicted to be empty, and the interesting
-- question is what `ucx_rkey` does in a capture where nothing one-sided
-- happened.
--
-- WHY IT MIGHT NOT BE EMPTY, AND WHY THAT IS WORTH ASKING. UCP's rendezvous
-- protocol exchanges a packed remote key on every large transfer so the receiver
-- can pull the buffer, and every message GROMACS sends here is large -- round 1
-- measured 72.5 GB across 103,292 sends, an average of 700 KB. Whether that
-- internal unpack passes through the public `ucp_ep_rkey_unpack` symbol a
-- provider can interpose, or through an internal one it cannot, is not something
-- any document available to a user of this tool says. This run answers it, in
-- the only direction that is cheap: if the table is non-empty on a workload with
-- no one-sided calls, the keys are UCX's own.
--
-- THE FIRST CLAUSE IS CASE 22'S CLAIM, ARRIVING FROM THE OTHER SIDE. There it
-- was "an RMA row names an rkey that no ucx_rkey row created", asserted on a
-- workload built to issue RMA. Here the workload issues none, so the same join
-- is being asked whether it stays sound when the population is whatever UCX
-- created for itself rather than whatever MPI created on the application's
-- behalf. A capture where those two differ is one where the entity table tracks
-- the API surface rather than the keys in use.
--
-- THE SECOND CLAUSE IS A FINDING FLAG, NOT A BUG. Keys with no consuming
-- operation are not wrong -- they are unattributable, which is a different and
-- more useful thing to say. The row it emits carries the count, so a reader can
-- see at once whether this is a handful of wireup keys or one per message.
SELECT 'an RMA row names an rkey that no ucx_rkey row created' AS problem,
       r.rank, r.rkey, count(*) AS rma_rows
FROM ucx_rma r
WHERE r.rkey <> 0
  AND NOT EXISTS (SELECT 1 FROM ucx_rkey k
                  WHERE k.rank = r.rank AND k.rkey = r.rkey)
GROUP BY r.rank, r.rkey

UNION ALL

SELECT 'the capture holds remote keys though the application issued no one-sided operation',
       -1,
       (SELECT count(*) FROM ucx_rkey),
       (SELECT count(*) FROM ucx_rma)
WHERE NOT EXISTS (SELECT 1 FROM ucx_rma)
  AND EXISTS (SELECT 1 FROM ucx_rkey);
