# PLAN — a board must prove its own provenance

- **Created:** 2026-09-04 19:00 +03
- **Status:** L1 landed (`d14354a`), validated on MN5. L2 and L3 outstanding.
- **Related:** `records.hpp` `Invocation` (532, the `images` scar),
  `link_sql.hpp` `run_provenance`, the retraction in `link_sql.hpp` `edge_sizes`,
  `run_on_a100.sh`'s provenance header, `all_in_one.sbatch`'s two pre-flights.

## The problem, in one line

**Every capture already records what produced it, and nothing ever checks it.**

`Invocation` carries `build` (commit, dirty flag, build type, engine ABI, schema
hash) and `images` (path, size and mtime of every mapped libpcomm object).
`run_provenance` exposes both as a derived table. No assertion anywhere compares
either against what the board claims to be testing. They are detective controls
that nobody reads, which is the same shape as a rubric row nobody counts.

## Why this is worth a plan rather than a fix

It has produced *false findings*, not just wasted time. Four instances, three of
them already written into the tree by whoever got burned:

1. **A stale binary invented a planner bug.** `edge_sizes`' comment retracts a
   claim that DuckDB resolved one queue pair's two size classes to different
   peers. The source had been restored after a mutation test; two rebuild-skipping
   cycles ran afterwards, so the binary went on answering for the mutated tree.
   The symptom was real and the cause was a stale `.so`.

2. **A stale copy nearly refuted a correct change.** Job 45331793 reported
   `uct_binding.accepted = 0` on a UCX version the guard had just been widened to
   admit. That job loaded `.so` files hand-copied into a second directory; the
   copy was two hours stale while the build tree beside it was correct. This is
   why `Invocation.images` exists at all.

3. **A mixed tree judged a fixed provider by a stale assertion.** `run_on_a100.sh`
   records it: the provider `.cpp` had landed and the `.sql` beside it *in the
   same commit* had not, because the checkout is fed by syncthing and files
   arrive independently.

4. **The gate went green over code it had never compiled** (2026-09-04, job
   45405085). Different axis, same family: what ran was not what the board
   claimed. Closed by the provider pre-flight in `6cdd9ed`, but only for that
   axis.

## Why the existing defences do not close it

- `run_on_mn5.sh` refuses a dirty tree and pushes `HEAD` over git, so a board is
  attributable to a commit — **but `--rsync` and `--no-build` both bypass that
  guarantee**, and both are used constantly while iterating. `--no-build` in
  particular means "the binaries are from whenever, the tree is from now", and
  the driver says nothing about the gap.
- `PCOMM_EXPECT_COMMIT` exists in `run_on_a100.sh` and checks the *local*
  checkout, not what the capture was produced by.
- `common.sh` overwrites an exported `PCOMM`, so a hand-run case silently uses
  whatever `PCOMM_BUILD` points at. Already a memory note; still a live trap.
- The stream header's schema hash catches a capture/reader mismatch, which is a
  different and narrower thing than a binary/source mismatch.

The common failure: **every one of these is discoverable after the fact and
none of them is asserted.**

## The design — three layers, most valuable first

### L1. The board asserts its own commit

The driver already knows the commit it synced; `run_on_mn5.sh` prints
`sync (git) main@<sha>`. Export it as `PCOMM_EXPECT_COMMIT`, let it ride
`--export=ALL` into the job, and have `sql_check.sh` materialise it, then assert.

    suite/asserts/build_is_the_boards_commit.sql
      -- requires: run_provenance
      SELECT rank, build FROM run_provenance, provenance
      WHERE provenance.expect_commit <> ''
        AND build NOT LIKE '%' || provenance.expect_commit || '%';

**The one structural obstacle:** `params` is INTEGER-only — `sql_check.sh`
generates `CREATE TABLE params (<col> INTEGER, ...)` from the `workload` block —
so a commit hash cannot live there. This needs a *second* one-row table
(`provenance`), materialised from the environment rather than from `meta.json`,
because it is a property of the run and not of the workload.

**It must fail, not skip, when `run_provenance` is empty.** A capture written
before `Invocation` existed, or one whose invocation record was lost, has
*unknown* provenance — which is exactly the state this assertion exists to
refuse. Empty-means-pass here would make the check vacuous on the captures most
likely to be stale. Pair it with a `NOT EXISTS` companion, per
`suite/README.md`'s vacuous-pass section.

### L2. No mapped pcomm image older than the engine

L1 cannot see instance (2) above, because the build id is compiled into the
**engine** and a stale *provider* `.so` linked against it reports the engine's
commit quite happily. `Invocation.images` is the field that can: it walks
`/proc/self/maps` at emit time and records `path size mtime` per line for every
mapped libpcomm object.

    assert: no mapped pcomm image has an mtime older than the engine's own

Catches the hand-copied-provider case directly. Needs string splitting over the
interned `images` blob (DuckDB `string_split` + `regexp_extract`), and a
tolerance decision: images built in one `cmake --build` differ by seconds, so the
threshold is "older than the engine by more than N minutes", and N must be
chosen from measured build spreads rather than guessed.

### L3. Refuse to run a build older than its source

L1 and L2 are still detective — they tell you afterwards, on a board you have
already paid for. The preventive form: before running, compare the mtime of
every built artifact against the newest tracked source file, and **refuse**
rather than warn.

That is what would make `--no-build` safe rather than dangerous: it stops meaning
"trust me" and starts meaning "don't rebuild if nothing changed". Note this is a
guard on the *driver*, so it also covers the `run_on_a100.sh` rsync path, which
has no commit to check against at all.

## What NOT to do

**Do not remove `--no-build` or `--rsync`.** Both exist because the alternative —
a full rebuild and a commit per iteration — is what makes people stop running the
gate at all. The goal is to make them *safe*, not to forbid them. A guard that
costs a rebuild every time will be disabled within a week, and then nothing is
checked at all; that is the same dynamic that makes a noisy rubric row get the
whole table ignored (`ucx_edges_include_intranode`).

**Do not put the commit in `params`.** It is not a workload parameter and typing
it as INTEGER would be a lie. A second table is the honest shape.

## Steps

1. `sql_check.sh`: materialise a one-row `provenance(expect_commit TEXT)` from
   the environment; empty string when unset, so the assertion is inert on a
   hand-run rather than red.
2. `run_on_mn5.sh` / `run_on_a100.sh`: export `PCOMM_EXPECT_COMMIT` from the
   commit they actually synced. For `--rsync` and the a100 rsync path, export the
   local `HEAD` **plus a dirty marker**, and make the assertion accept the marker
   only when it is present on both sides.
3. `suite/asserts/build_is_the_boards_commit.sql` + its non-empty companion.
4. Mutation-test both: point a board at a deliberately stale build tree and
   confirm it goes red; then delete `run_provenance` and confirm it *still* goes
   red rather than skipping.
5. L2 and L3 as separate changes, each with its own measurement.

## What L1 turned out to be (2026-09-04, `d14354a`)

Landed as described, with two deviations and both are worth keeping.

**It reads `invocation`, not `run_provenance`.** `run_provenance` is a DERIVED
table, and MN5 builds with `PCOMM_DUCKDB=OFF` whenever the shipped libduckdb is
not there — so the assertion would have been invisible on the one board it
exists to protect. The CSV export resolves Invocation's handles to text itself
(`pcomm-read/main.cpp`, `Table<Invocation>`), so the sqlite path can answer.
The Parquet re-run never fires for it, because `pq_only` skips any assertion
whose tables the CSV set already has — which is right: on that path `build` is
still an intern handle.

**An unset runner variable is a `[skip]`, not an inert `[ok]`.** `provenance`
has zero rows when nobody named a commit, so `-- requires: provenance` prints a
visible skip on a hand-run, and every board prints one `provenance:` line
saying whether the check is armed.

### Answers to the open questions

- **`build`'s format** is `commit=<sha12>[-dirty] type=<cfg> abi=<n>
  schema=0x<16 hex>`. The match anchors on BOTH sides — `commit=` and ` type=` —
  so a 7-character `rev-parse --short` cannot pass against the 12 characters
  `write_build_id.cmake` stamps. Worth knowing: MN5's build stamps `type=unset`,
  because no `CMAKE_BUILD_TYPE` is passed there.
- **Dirty** must agree on both sides and is not a note. The git path refuses a
  dirty tree, so its boards name a bare commit; `--rsync` and `run_on_a100.sh`
  stamp the build with the same `-dirty` id they claim, via `$PCOMM_COMMIT`. A
  build calling itself dirty under a runner claiming a clean commit is a
  different tree and says so.

### Validated by mutation, on the cluster

- Job **45425770**: board claims `4e8d9bf2a24f`, remote tree synced to it,
  `--no-build` left the binaries at `d14354a3eb9c`. Every case that produced a
  capture went red naming both ids. The four hardware skips stayed skips — the
  assertion does not turn an honest "this host cannot" into a failure.
- Locally, against synthetic captures: a 7-char prefix, a different commit and a
  clean/dirty disagreement all red; an `invocation` table that is empty or
  absent red rather than skipped; unset runner variable `[skip]`.

### What it cost to find out, which is the thesis restated

The first board after L1 (job **45425560**) produced no board at all: the
provider pre-flight from `6cdd9ed` reported all 23 cases as build holes on a
build that had just compiled every one of them. `$PCOMM` is `common.sh`'s to
define and `all_in_one.sbatch` never sourced it. Sourcing it would still not
have been enough — without `pcomm_load_runtime` the launcher dies in `ld.so` for
want of `GLIBCXX_3.4.30` and exits **1**, the same code the launcher uses for
"provider .so not found". Fixed in `4e8d9bf` with a positive control
(`pcomm list`) so a check that cannot start says so instead of voting no.

That is this plan's argument in a third form: a gate that cannot run and a gate
that ran clean must not look the same.

## What is left

- **L2 and L3 as written below.** L3's preventive half is blocked on something
  small: no binary exposes its build id. `pcomm_build_id.h` is private to the
  engine target and the CLI has no `--version`, so a runner cannot ask "which
  commit is this build" without producing a capture first.
- **Nothing forces a runner to arm the check.** Both drivers export
  `PCOMM_EXPECT_COMMIT`, and a board whose export silently stopped working would
  report `[skip]` on every case and still go green. The `provenance:` line makes
  it visible; nothing makes it required. A run-level assertion — "no case may
  skip this one" — needs `result.json` to name skipped assertions, which it does
  not today.

## Open questions

- Would shipping the runner and its dependencies as one self-contained thing
  (static libstdc++, statically linked DuckDB) remove most of this surface? Half
  of today's two failures were dependency resolution rather than logic:
  `GLIBCXX_3.4.30` from an unloaded module, and a derived-table layer that is
  absent on MN5 only because libduckdb has to be pushed by rsync. Its own plan,
  but it touches every conclusion here.
