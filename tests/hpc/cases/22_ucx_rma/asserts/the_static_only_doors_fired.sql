-- requires: ucx_rma ucx_atomic ucx_tag_send params.rma_iters params.ssend_iters
-- The doors that were hooked on a static ELF survey and never observed.
--
-- PLAN-20260815-1540 *Future work 2* names this exactly: everything added on
-- 2026-08-16 — the `_nbi` RMA doors, the atomics, the legacy tagged doors — is
-- hooked on a static reference, "which is good evidence that the code path
-- exists and no evidence at all that it is taken". This is the assertion that
-- converts one into the other.
--
-- THREE SEPARATE CLAIMS, deliberately not merged into one EXISTS. A single
-- combined check would go green as soon as any one door fired, which is the
-- failure mode that let three of ucx_deep's four op families sit unwitnessed
-- behind a passing case.
--
-- `nbi` and not `nbx`: the survey found that MN5's MPI one-sided path calls
-- ucp_put_nbi/ucp_get_nbi and NO `_nbx` RMA call at all. A capture showing
-- `nbx` here would mean the stack changed under us, which is worth failing over
-- rather than quietly accepting.
SELECT 'MPI_Put/MPI_Get did not reach the _nbi doors' AS problem
WHERE (SELECT rma_iters FROM params) > 0
  AND NOT EXISTS (SELECT 1 FROM ucx_rma WHERE door = 'nbi')
UNION ALL
-- NOT "ucx_atomic is non-empty", and job 45177421 is why. That run's atomic
-- table held 80 rows and every one of them came from mca_osc_ucx's OWN window
-- synchronisation -- successful 8-byte `cswap` and `swap` -- while all 20 of
-- the workload's own operations failed with status -5, because they asked for a
-- ONE-BYTE atomic and UcxAtomic.op_size means it when it says "4 or 8". A
-- non-empty check would have gone green over a workload whose every atomic was
-- refused, which is the precise shape of passing for the wrong reason.
--
-- So: an `add`, and a SUCCEEDING one. MPI's internal locking uses cswap and
-- swap and never add, so `add` is attributable to MPI_Accumulate and
-- MPI_Fetch_and_op and to nothing else in this workload.
SELECT 'MPI_Accumulate/MPI_Fetch_and_op produced no SUCCESSFUL add' AS problem
WHERE (SELECT rma_iters FROM params) > 0
  AND NOT EXISTS (SELECT 1 FROM ucx_atomic
                  WHERE op = 'add' AND status_class <> 'error')
UNION ALL
-- And the failures themselves are worth failing over rather than tolerating.
-- A refused atomic is the tracer working and the workload broken; leaving it
-- unasserted is how the defect above survived a whole queue slot.
SELECT 'the workload issued atomics the transport refused' AS problem
WHERE EXISTS (SELECT 1 FROM ucx_atomic WHERE status_class = 'error')
UNION ALL
SELECT 'MPI_Ssend did not reach the legacy synchronous tagged door' AS problem
WHERE (SELECT ssend_iters FROM params) > 0
  AND NOT EXISTS (SELECT 1 FROM ucx_tag_send WHERE door = 'sync_nb');
