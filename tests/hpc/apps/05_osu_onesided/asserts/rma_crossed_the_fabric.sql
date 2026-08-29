-- requires: transport_mix params.total_ops
-- MPI_Put and MPI_Get must arrive at the transport as Put*/Get*, over the
-- fabric, on a machine where there is no shared memory to arrive over.
--
-- TWO CLAIMS, and the second is the one that cannot be got by looking at the
-- UCP layer alone.
--
--   * The doors. MPI one-sided reaches UCT's RMA doors ONLY through the UCX
--     one-sided component. With osc/rdma or osc/pt2pt the very same MPI_Put
--     becomes an active message, `remote_addr` is never written by anything,
--     and the capture degrades to something that looks exactly like a correct
--     capture of a different program. job.sbatch asks for `--mca osc ucx`
--     explicitly; this is what notices when the request was not honoured.
--   * The transport. The job runs ONE RANK PER NODE, so the pair has no
--     shared-memory path available to it: there is no second rank on this node
--     to reach through sysv, posix, cma or xpmem. A one-sided operation
--     attributed to one of those transports is therefore not a slow path, it is
--     a misattribution — the interface pointer resolved to the wrong
--     uct_iface, which is exactly the failure link_health's
--     unnamed_uct_transport counts when it can detect it and this catches when
--     it cannot.
--
-- The op vocabulary is named here and nowhere else in this directory, because
-- transport_mix is a DERIVED table and case 21's assertions establish that it
-- carries decoded names on both engines, where the raw ucx_* tables do not.
SELECT 'a one-sided operation took a shared-memory transport, on a run with no same-node peer' AS problem,
       rank,
       CAST(tl_name AS VARCHAR) AS tl_name,
       CAST(op AS VARCHAR)      AS op,
       ops
FROM transport_mix
WHERE (CAST(op AS VARCHAR) LIKE 'put%' OR CAST(op AS VARCHAR) LIKE 'get%')
  AND lower(CAST(tl_name AS VARCHAR)) IN ('self', 'sysv', 'posix', 'cma', 'xpmem', 'knem', 'mm')
UNION ALL
SELECT 'the workload issued one-sided operations but no Put*/Get* door was recorded',
       NULL, NULL, NULL,
       (SELECT total_ops FROM params)
WHERE (SELECT total_ops FROM params) > 0
  AND NOT EXISTS (SELECT 1 FROM transport_mix
                  WHERE CAST(op AS VARCHAR) LIKE 'put%'
                     OR CAST(op AS VARCHAR) LIKE 'get%');
