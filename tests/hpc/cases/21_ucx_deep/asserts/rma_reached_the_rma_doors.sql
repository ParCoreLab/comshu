-- requires: transport_mix params.rma_iters
-- MPI_Put and MPI_Get must arrive as Put*/Get*, not as active messages.
--
-- The one-sided half of the workload exists to reach doors nothing else can:
-- `remote_addr` is written by Put*/Get* and by no other UCT operation, and it is
-- the object-heatmap key the whole HPC-X class has never had — the same key
-- `ib_post_send.remote_addr` gives for raw verbs.
--
-- It is also the claim most likely to fail for a reason that is not comshu's.
-- MPI one-sided reaches UCT's RMA doors only through the UCX one-sided
-- component; with osc/rdma or osc/pt2pt the same MPI_Put becomes an active
-- message and no remote address is ever recorded. The job script asks for
-- `--mca osc ucx` explicitly, and this assertion is what notices when the
-- request was not honoured — a capture that silently degraded to active
-- messages otherwise looks exactly like a correct one.
SELECT 'the workload issued RMA but no Put*/Get* door was recorded' AS problem,
       (SELECT rma_iters FROM params) AS rma_iters,
       (SELECT rma_bytes FROM params) AS rma_bytes
WHERE (SELECT rma_iters FROM params) > 0
  AND NOT EXISTS (SELECT 1 FROM transport_mix
                  WHERE op IN ('put_short', 'put_bcopy', 'put_zcopy',
                               'get_short', 'get_bcopy', 'get_zcopy'));
