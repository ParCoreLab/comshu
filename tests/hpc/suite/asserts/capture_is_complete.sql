-- Every rank finished, and none of them lost a record on the way.
--
-- DELIBERATELY NO `-- requires:` LINE. A requires-guard skips the assertion when
-- the named table is absent, and `capture_status` absent is not a case to skip —
-- it is the worst capture this file can be handed. Guarding on it would make
-- this pass most quietly on a run where no rank finished at all. That exact
-- vacuous pass shipped once already: `-- requires: nccl_coll_exec` turned a
-- capture that had lost its entire profiler layer into a green [skip].
-- A capture with no capture_status table fails here with "no such table", which
-- is the right answer stated badly rather than the wrong answer stated well.
--
-- CROSS-CUTTING ON PURPOSE, and it is the assertion that decides whether the
-- others mean anything. Every other check in this directory reads tables and
-- concludes something about the workload; all of them are reading a capture
-- they assume is whole. On MN5 job 45351029 that assumption was false — the home
-- quota was exhausted, streams were written headerless or not at all — and three
-- cases went red pointing at InfiniBand and UCX behaviour while the greens
-- beside them were equally meaningless. An hour went to the wrong layer.
--
-- all_in_one.sbatch already fails a case whose LOG says CAPTURE INCOMPLETE. This
-- is the same claim made against the capture instead of against the log, which
-- matters for the case the log cannot cover: a rank that was killed and never
-- printed anything at all.
--
-- THE ABSENCE HALF IS THE POINT. capture_status is written only by a normal
-- finalize(), so a rank in clock_anchor with no row here started and did not
-- finish. A loss counter cannot express that — the process that would have
-- written the count is the one that died — which is why this assertion is a
-- LEFT JOIN from clock_anchor rather than a scan of capture_status. Written the
-- other way it would pass most confidently on the capture that lost the most.
SELECT problem, rank, detail
FROM (
    SELECT 'rank started but never finished — capture is short by an unknown amount'
               AS problem,
           a.rank AS rank,
           'no capture_status row' AS detail
    FROM (SELECT DISTINCT rank FROM clock_anchor) a
    LEFT JOIN capture_status s ON s.rank = a.rank
    WHERE s.rank IS NULL

    UNION ALL

    SELECT 'tracer could not write part of this rank''s capture',
           s.rank,
           CAST(s.records_lost AS VARCHAR) || ' record(s), ' ||
           CAST(s.bytes_lost AS VARCHAR) || ' byte(s) lost across ' ||
           CAST(s.streams_lost AS VARCHAR) || ' stream(s)'
    FROM capture_status s
    WHERE s.records_lost > 0 OR s.bytes_lost > 0 OR s.streams_lost > 0

    UNION ALL

    -- Distinct from the row above because it is a different failure with a
    -- different cause: not a stream that lost records, but a stream that has no
    -- header at all (unreadable to every reader) or that could never be opened.
    SELECT 'a stream in this rank is unreadable or was never created',
           s.rank,
           CASE WHEN s.dir_failed > 0 THEN 'the output directory could not be created'
                WHEN s.header_lost > 0 THEN 'a stream has no header and cannot be parsed'
                ELSE 'a stream file could not be opened' END
    FROM capture_status s
    WHERE s.dir_failed > 0 OR s.header_lost > 0 OR s.never_opened > 0
)
ORDER BY rank, problem
