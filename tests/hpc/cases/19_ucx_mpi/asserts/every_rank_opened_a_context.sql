-- requires: ucx_context ucx_worker params.ranks
-- Every rank must have opened a UCP context AND a worker.
--
-- This is the case's floor, not its ceiling: it is the assertion that would
-- have been red for the whole life of the UCX provider, because until case 19
-- existed no suite case loaded it and every ucx_* table in the corpus was empty
-- on every capture. A provider that builds, passes a byte-level schema test and
-- a 0.08 s local gate, and never records a row on a cluster, looks exactly like
-- a working provider from inside the repo.
--
-- Derived from params.ranks, so it does not name a rank and does not care which
-- one is which. Stated as "fewer than expected": more would mean the workload
-- changed, fewer means a rank's interposition never engaged — which is the
-- failure that matters and the one an "at least one row" check cannot see.
SELECT 'not every rank opened a UCP context' AS problem,
       (SELECT count(DISTINCT rank) FROM ucx_context) AS ranks_with_context,
       (SELECT ranks FROM params)                     AS expected
WHERE (SELECT count(DISTINCT rank) FROM ucx_context) < (SELECT ranks FROM params)
UNION ALL
SELECT 'not every rank opened a UCP worker',
       (SELECT count(DISTINCT rank) FROM ucx_worker),
       (SELECT ranks FROM params)
WHERE (SELECT count(DISTINCT rank) FROM ucx_worker) < (SELECT ranks FROM params);
