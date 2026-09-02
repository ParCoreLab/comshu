-- requires: nccl_comm nccl_collective
-- The profiler-plugin door opened, and what came through it joins.
--
-- This is the assertion the whole 912-916 record block exists for. Everything
-- in 900-910 is what the application ASKED for; nothing there can say whether
-- NCCL ran a ring or a tree, and the difference is roughly half the wire volume
-- (case 02's meta.json: pinning Ring/Simple/1-channel moved it from 57,280
-- sends to 26,880).
--
-- `nccl_coll_exec` IS DELIBERATELY ABSENT FROM THE REQUIRES LINE, for the same
-- reason every_rank_says_what_it_was_asked_to_do.sql omits `invocation`: an
-- empty table IS the defect here, and `-- requires:` skips on empty as well as
-- absent, so naming it would turn the failure into a [skip] and the board back
-- to green. The CSV export writes a header-only file for a stream with no
-- records, so the query below still parses.
--
-- This was MEASURED, not reasoned. The first version of this file did require
-- the table; re-running case 20 with NCCL_PROFILER_PLUGIN=none produced
-- `[skip] nccl_stated_what_it_chose` and `=== 20_nccl_collectives : PASS ===`
-- over a capture that had lost the entire profiler layer —
-- link_health.nccl_profiler_door_shut read 32 and nothing turned red.
--
-- `nccl_comm` and `nccl_collective` stay required: without them there is no
-- NCCL in the capture at all, and a case that ran no collectives genuinely
-- cannot speak to any of this.
--
-- THE PINNING IS WHAT MAKES THE ALGO CHECK POSSIBLE AND ALSO WHAT LIMITS IT.
-- job.sbatch sets NCCL_ALGO=Ring NCCL_PROTO=Simple, so the expected value is
-- known and an equality check is legitimate. On an unpinned run it would not be,
-- and asserting a constant there would be asserting our guess rather than NCCL's
-- choice. Note the case fold: NCCL reports "RING"/"SIMPLE" uppercased while the
-- environment variable is spelled "Ring"/"Simple", and comparing them raw fails
-- on a capture that is entirely correct.
SELECT * FROM (
  -- Every rank that made a collective call must also have said what NCCL did
  -- with it. A rank present in one table and missing from the other means the
  -- plugin loaded for some ranks and not others, which is worse than not
  -- loading at all: the board would be green over a partial answer.
  SELECT 'rank called collectives but recorded no execution rows' AS problem,
         CAST(c.rank AS VARCHAR)                                  AS detail
  FROM (SELECT DISTINCT rank FROM nccl_collective) c
  LEFT JOIN (SELECT DISTINCT rank FROM nccl_coll_exec) x ON x.rank = c.rank
  WHERE x.rank IS NULL

  UNION ALL
  -- The pinned algorithm, folded for case. Anything else means either NCCL
  -- ignored the pin or we are reading the wrong field of the descriptor — and
  -- the second is the one to fear, because the union arms in a profiler event
  -- descriptor all look like plausible strings.
  SELECT 'algo is not the pinned Ring', algo
  FROM nccl_coll_exec WHERE upper(algo) != 'RING'

  UNION ALL
  SELECT 'proto is not the pinned Simple', proto
  FROM nccl_coll_exec WHERE upper(proto) != 'SIMPLE'

  UNION ALL
  -- An empty algo or proto would pass the two checks above only if the string
  -- were literally "RING"/"SIMPLE", so this is not redundant — it catches the
  -- case where the handle resolved to nothing and the row is a shell.
  SELECT 'algo or proto is empty', CAST(rank AS VARCHAR)
  FROM nccl_coll_exec WHERE algo = '' OR proto = ''

  UNION ALL
  -- THE CROSS-LAYER JOIN. nccl_comm.nccl_comm_hash is OUR reimplementation of
  -- NCCL's getHash over the ncclUniqueId; nccl_coll_exec.nccl_comm_hash is the
  -- number NCCL handed the plugin. They are computed by different code in
  -- different places and must be equal. If they are not, either the hash
  -- reimplementation drifted from NCCL's or the argument order of the plugin's
  -- init was misread — the latter compiles silently, since v4 and v5 order
  -- (mask, commName, commHash) differently.
  SELECT 'execution row names a communicator the host API never recorded',
         CAST(x.nccl_comm_hash AS VARCHAR)
  FROM nccl_coll_exec x
  LEFT JOIN nccl_comm c ON c.rank = x.rank AND c.nccl_comm_hash = x.nccl_comm_hash
  WHERE c.rank IS NULL

  UNION ALL
  -- Zero would join every communicator to every other one. On this workload the
  -- comm is built by ncclCommInitRank, so the hash IS derivable and a zero is a
  -- defect — unlike a capture using ncclCommSplit, where zero is the honest
  -- answer and link_health counts it instead.
  SELECT 'nccl_comm_hash is zero on a plain ncclCommInitRank workload',
         CAST(rank AS VARCHAR)
  FROM nccl_comm WHERE nccl_comm_hash = 0

  UNION ALL
  -- The two doors must agree on how many collectives ran. They count the same
  -- calls from opposite ends — one on the host thread as the API returns, one
  -- inside the enqueue — so a mismatch means one of them is dropping work.
  SELECT 'the two NCCL doors disagree on the collective count',
         CAST((SELECT count(*) FROM nccl_coll_exec) AS VARCHAR) || ' exec vs ' ||
         CAST((SELECT count(*) FROM nccl_collective) AS VARCHAR) || ' calls'
  WHERE (SELECT count(*) FROM nccl_coll_exec) != (SELECT count(*) FROM nccl_collective)

  UNION ALL
  -- An interval that ends before it starts is the signature of reading the
  -- descriptor at stopEvent instead of caching it at startEvent.
  SELECT 'execution interval runs backwards', CAST(event_id AS VARCHAR)
  FROM nccl_coll_exec WHERE end_ticks < ticks
)
ORDER BY problem, detail;
