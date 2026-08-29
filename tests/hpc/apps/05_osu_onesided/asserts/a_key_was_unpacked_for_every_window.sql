-- requires: ucx_rma ucx_rkey params.n_windows
-- Five windows were created; the origin cannot write into a window whose key it
-- never unpacked.
--
-- THE ORACLE IS THE APPLICATION'S STRUCTURE, not a count from a capture.
-- run_put_with_flush calls allocate_memory_one_sided once per message size
-- (osu_put_bw.c:289) and free_memory_one_sided at the end of the same iteration
-- (:348), and with `-m 512:8192` the size loop runs five times. Five separate
-- MPI_Win_allocate / MPI_Win_free cycles, five separate remote registrations,
-- and remote memory reached through a key that must be unpacked for each.
--
-- A FLOOR, NOT AN EQUALITY, and the distinction is the point. OpenMPI's osc/ucx
-- keeps a state window beside each data window, so I expect roughly ten rows
-- and assert five: how many keys an MPI unpacks per window is its own business
-- and freezing it here would make this study a golden of OpenMPI's internals.
-- What is NOT the MPI's business is unpacking fewer keys than it created
-- windows — that is arithmetically impossible for a workload that wrote into
-- every one of them, and any capture showing it has lost rows.
--
-- WHAT IT CATCHES that every_rma_key_resolves cannot: a provider that records
-- the FIRST unpack and misses the rest. Every RMA row would still resolve —
-- to a recycled handle from window one — and the join would look perfect while
-- four windows' worth of keys had vanished.
--
-- Scoped to ranks that actually issued RMA, so it cannot fail on the passive
-- target, which legitimately unpacks nothing: OpenMPI unpacks a peer's key
-- lazily on first access and rank 1 never accesses anything.
SELECT 'the origin rank unpacked fewer keys than it created windows' AS problem,
       r.rank                                                        AS rank,
       (SELECT count(*) FROM ucx_rkey k WHERE k.rank = r.rank)       AS keys_unpacked,
       (SELECT n_windows FROM params)                                AS windows_created
FROM (SELECT DISTINCT rank FROM ucx_rma) r
WHERE (SELECT count(*) FROM ucx_rkey k WHERE k.rank = r.rank)
      < (SELECT n_windows FROM params);
