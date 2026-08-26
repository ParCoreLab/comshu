-- requires: ucx_tag_send params.reneighbour_every
-- The halo message size is FROZEN between neighbour-list rebuilds, and the
-- length of the run it is frozen for is the input's reneighbour period.
--
-- in.lj says `neigh_modify delay 0 every 20 check no`, so the ghost set is
-- rebuilt on exactly every 20th step and on no other.  On a rebuild step
-- Verlet::run calls exchange() + borders() and NOT forward_comm; on the other
-- 19 steps of the period forward_comm re-sends the same ghost list
-- (src/comm_brick.cpp:497 packs sendlist[iswap], which borders() last wrote),
-- so the byte count on a given swap is bit-identical 19 times in a row.
--
-- The claim: the run length that accounts for MORE HALO MESSAGES THAN ANY
-- OTHER is reneighbour_every - 1.  Weighted by messages, not by how many
-- distinct sizes have that run length -- the sizes that occur exactly once are
-- numerous (the borders()/exchange() traffic at each rebuild, which is a
-- different size every time) but they carry few messages between them.  It
-- reads an INPUT PARAMETER back out of the wire and names no rank, endpoint or
-- size the run happened to produce.
-- RESTATED ONCE, 2026-08-26, and the first version was WRONG rather than
-- inconvenient.  It read "the most common run length", encoded as
-- ORDER BY count(*) DESC over the run-length groups -- which counts DISTINCT
-- SIZES, not messages, and so answered 1: the borders()/exchange() traffic at
-- each rebuild is a different byte count every time, so it contributes many
-- run-length-1 groups (387 of them) while carrying 387 of 4968 halo messages.
-- The claim was always about where the MESSAGES are, so the fix is
-- ORDER BY sum(n) DESC.  Measured after the change: run length 19 carries 4389
-- of 4968, run length 1 carries 387.  Recorded here rather than silently
-- amended, per tests/hpc/suite/README.md -- an assertion is never edited to
-- match what the tool emitted, only when an independent reading says the
-- encoding did not say what the claim says.
--
SELECT modal_run_length, messages
FROM (
  SELECT n AS modal_run_length, sum(n) AS messages
  FROM (
    SELECT s.rank, s.endpoint, s.count AS sz, count(*) AS n
    FROM ucx_tag_send s
    WHERE s.count >= (SELECT halo_min_bytes FROM params)
    GROUP BY 1, 2, 3
  ) runs
  GROUP BY n
  ORDER BY sum(n) DESC
  LIMIT 1
) top
WHERE modal_run_length != (SELECT reneighbour_every FROM params) - 1
