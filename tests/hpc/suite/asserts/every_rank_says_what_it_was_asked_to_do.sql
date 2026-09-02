-- requires: clock_anchor
-- Every rank that recorded anything must carry exactly one invocation row.
--
-- DELIBERATELY NOT `-- requires: invocation`, and that is the whole design of
-- this file. An empty `invocation` table is precisely the defect being guarded
-- against, and requiring it would turn that defect into a [skip] — an assertion
-- that reports "the capture cannot speak to this" about the one thing it exists
-- to check. So it requires `clock_anchor` instead: the engine writes that on
-- every rank at start, so it means "this capture has an engine" without naming
-- the thing under test.
--
-- WHAT WENT WRONG, so the next person does not re-derive it. Invocation (532)
-- was first emitted only from the arena-flush path, which fires when a record
-- buffer FILLS. On MN5 job 45344306 that left hubspoke_ib (41,721 sends) and
-- nvshmem_multinode with their rows and ucx_deep, ib_pingpong (40 sends) and
-- ib_send_bw (1,000) with NONE. The suite was green throughout, because nothing
-- looked. Tracer::finalize now emits it as a backstop.
--
-- Clause 2 is not redundant with clause 1. A duplicate row would mean the
-- once-only gate had failed — both call sites firing, or the fork handler
-- re-arming it — and `run_provenance` would then report a rank twice, which a
-- reader joining on rank would silently multiply.
SELECT 'rank recorded work but has no invocation row' AS problem,
       r.rank                                        AS rank,
       0                                             AS rows
FROM ranks_seen r
LEFT JOIN invocation i ON CAST(i.rank AS INTEGER) = r.rank
WHERE i.rank IS NULL

UNION ALL

SELECT 'rank has more than one invocation row',
       rank,
       n
FROM (SELECT CAST(rank AS INTEGER) AS rank, count(*) AS n
      FROM invocation GROUP BY 1 HAVING count(*) > 1);
