-- requires: ucx_endpoint ucx_worker
-- Referential integrity, on the join the whole UCX record set hangs from.
--
-- ucx_endpoint carries `worker`, ucx_worker carries `context`, and every
-- question worth asking of this provider walks that chain — "which peer did
-- this endpoint reach", "what did this context do". An endpoint naming a worker
-- the capture never recorded means the chain is broken at its first link, and
-- every downstream answer is silently built on a dangling handle.
--
-- Names no rank, no handle and no count: this is an invariant, so it holds for
-- any workload on any node count, and it is the assertion most likely to catch
-- an interposition that engaged halfway (worker doors missed, endpoint doors
-- hit) rather than not at all.
--
-- Handles are interned per rank, so the join MUST carry rank. Joining on the
-- handle alone would let rank 0's worker satisfy rank 1's endpoint and turn a
-- real break into a pass.
SELECT 'endpoint names a worker this capture never recorded' AS problem,
       e.rank, e.endpoint, e.worker
FROM ucx_endpoint e
LEFT JOIN ucx_worker w ON w.rank = e.rank AND w.worker = e.worker
WHERE w.worker IS NULL;
