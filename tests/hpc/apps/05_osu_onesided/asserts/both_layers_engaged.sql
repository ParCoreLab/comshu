-- requires: uct_binding ucx_rma
-- Two hook-based providers, one process, one pass — and the deep one must have
-- DARED to patch, without being told to.
--
-- `comshu run -p ucx,ucx_deep` puts two non-exclusive providers in one engine.
-- The half that can fail quietly is ucx_deep: it reaches UCX's data plane
-- through uct_iface_ops_t, an installed header with no ABI promise across
-- versions, and it guards that by comparing the table size it compiled against
-- with the UCX actually loaded. accepted = 0 is the honest empty capture —
-- uct_send has no rows and uct_binding says why, rather than leaving a reader
-- to conclude that a benchmark which demonstrably moved 3 MB sent nothing.
--
-- forced = 1 means COMSHU_UCX_DEEP_FORCE overrode a refusal. Such a capture may
-- be perfectly correct, but every row in it rests on a guard that said no, and
-- a study must not report that as an ordinary pass. This job sets no such
-- variable, so a 1 here would also mean the environment is not what the script
-- thinks it is.
--
-- SECOND CLAUSE: the UCP layer recorded one-sided traffic and the UCT layer
-- recorded nothing at all, with the binding accepted. That combination is not
-- an empty capture, it is a contradiction — the application asked UCX to move
-- bytes to another node and the transport that carried them is missing. It is
-- the shape a co-run failure takes when one provider loses its interposition to
-- a library-ordering change while the other keeps its own.
--
-- Names no version and no ops_size: it asserts the provider's own verdict
-- rather than a particular UCX.
SELECT 'the ucx_deep layout guard refused, or was forced' AS problem,
       rank,
       compiled_version,
       runtime_version,
       accepted,
       forced
FROM uct_binding
WHERE CAST(accepted AS INTEGER) <> 1
   OR CAST(forced AS INTEGER) <> 0
UNION ALL
SELECT 'the UCP layer recorded one-sided traffic that the accepted UCT layer never saw',
       NULL, NULL, NULL, NULL, NULL
WHERE EXISTS (SELECT 1 FROM ucx_rma)
  AND EXISTS (SELECT 1 FROM uct_binding WHERE CAST(accepted AS INTEGER) = 1)
  AND NOT EXISTS (SELECT 1 FROM uct_send);
