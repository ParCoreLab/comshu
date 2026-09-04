-- requires: provenance
-- The binaries that produced this capture were built from the commit the runner
-- says this board is about.
--
-- WHY THIS IS THE FIRST ASSERTION ABOUT THE TOOL RATHER THAN THE WORKLOAD.
-- Every capture has always recorded the build that made it — Invocation.build
-- carries `commit=<sha>[-dirty] type=<cfg> abi=<n> schema=0x<hash>` — and
-- nothing ever read it. A detective control nobody reads is not a control, and
-- the gap has produced *false findings* four times, not merely wasted runs:
--
--   1. A stale .so invented a DuckDB planner bug (link_sql.hpp `edge_sizes`
--      retracts the claim). The source had been restored after a mutation test
--      and two rebuild-skipping cycles answered for the mutated tree.
--   2. A hand-copied provider two hours stale nearly refuted a correct change
--      (job 45331793, `uct_binding.accepted = 0` on a UCX the guard had just
--      been widened to admit). Invocation.images exists because of it.
--   3. A syncthing-fed checkout ran a mixed tree: the provider .cpp had landed
--      and the .sql beside it in the same commit had not (run_on_a100.sh).
--   4. The gate went green over a backend it had never compiled (job 45405085).
--
-- All four are the same sentence: what ran was not what the board claimed.
--
-- THE MATCH IS EXACT, ANCHORED ON BOTH SIDES. `commit=` pins the left, ` type=`
-- pins the right, so a 7-character `rev-parse --short` cannot pass against the
-- 12-character id write_build_id.cmake stamps. That near-miss — a prefix
-- matching for the wrong reason — is precisely how a provenance check ends up
-- green on a build it never identified.
--
-- The `-dirty` suffix is part of the id and must agree on both sides.
-- run_on_mn5.sh's git path refuses a dirty tree outright, so a board from it
-- names a bare commit; --rsync and run_on_a100.sh copy the working tree and
-- stamp the build with the same marker they claim here. A build calling itself
-- dirty under a runner that claims a clean commit is a different tree, and says
-- so here rather than in whatever finding it would otherwise have produced.
--
-- DELIBERATELY NOT `-- requires: invocation`. An absent or empty invocation
-- table is unknown provenance, which is exactly the state this assertion
-- exists to refuse — requiring it would turn the defect into a [skip]. If the
-- table is missing entirely sqlite fails the query and the case goes red, which
-- is the right direction and is also what every_rank_says_what_it_was_asked_to_do
-- already does with the same table.
--
-- `provenance` IS required, and it has zero rows when no runner named a commit
-- (sql_check.sh). So a hand-run reports [skip] — honest, and visibly different
-- from a pass.
SELECT 'capture was produced by a build of some other commit' AS problem,
       i.rank                                                AS rank,
       p.expect_commit                                       AS board_claims,
       i.build                                               AS capture_says
FROM invocation i, provenance p
WHERE coalesce(i.build, '') NOT LIKE 'commit=' || p.expect_commit || ' type=%'

UNION ALL

-- The companion the vacuous-pass rule asks for: the clause above is
-- empty-means-pass, and it is empty when there is nothing to check as well as
-- when everything checks out. A board that names a commit and holds no
-- invocation row cannot support the claim.
SELECT 'board names a commit but no rank recorded which build ran',
       NULL,
       p.expect_commit,
       NULL
FROM provenance p
WHERE NOT EXISTS (SELECT 1 FROM invocation);
