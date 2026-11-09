# NCCL chose the algorithm, we did not — the profiler plugin door

**Status: DONE.** Verified on du04 (single node) and MN5 job 45360625
(multi-node, 8 ranks over 2 nodes). Finding 9 is closed.

**Finding 9** from `PLAN-20260902-0300-lammps-and-llm-scaleout-findings-done.md`,
the last one open. This is its own session because it is a new door, not a field
on an existing record.

## What is missing

`nccl_collective` says what the application **asked for**: this communicator,
this operation, this many elements of this type, on this stream. It cannot say
what NCCL **decided** — Ring or Tree or NVLS, Simple or LL or LL128, over how
many channels, decomposed into which peer-to-peer steps. Those are chosen inside
`ncclEnqueue` from measured bandwidth and topology, after our interposer has
already returned.

The gap is not cosmetic. Case 02's own `meta.json` records that pinning
Ring/Simple/1-channel moved the wire volume from 57,280 sends to 26,880. The
choice is roughly half the traffic, and at the host-API door it is invisible.

**What we have in the meantime, and its exact limit.** `NCCL_ALGO` and
`NCCL_PROTO` are in the `Invocation` env allowlist, so a capture that *pins* them
says so. That is a record of what was **requested**. When they are unset — which
is every real workload — it says nothing, and no report may blur the two.

## The objection this has to answer

Raised directly, and correctly: *can we really link the NCCL calls to an
underlying data movement?* Sometimes the bytes go by GIN/IBGDA, sometimes by DMA
over NVLink, sometimes the API call only registers work that `ncclGroupEnd`
launches later. Too many variables to infer.

That objection is fatal to **inference** — reading ibverbs traffic and guessing
which collective produced it. It is not an objection to this door, because the
profiler API does not ask us to infer anything. NCCL **states** the parent-child
chain itself: every event descriptor carries a `parentObj`, and that pointer is
the handle we ourselves returned from `startEvent` for the parent. The chain
`groupApi → collApi → coll → proxyOp → proxyStep` is NCCL's own accounting, not
our reconstruction. `proxyOp` names a `peer`, a `channelId`, an `nSteps` and a
`chunkSize`, and `isSend`.

So the answer to "can the call be linked to the movement" is: NCCL will tell us,
for the paths that go through its proxy. What it will *not* tell us is anything
about device-initiated traffic that bypasses the proxy entirely — and this plan
must not claim otherwise. See "What this door still cannot see" below.

## Measured facts, not assumptions

Checked on the actual machines, and the first check was **against the wrong
library** — which is the most useful thing in this section.

| host | libnccl the CASES load (`$NCCL_HOME`) | version | shim symbols it looks up |
|---|---|---|---|
| MN5 | HPC SDK 26.5, `comm_libs/12.9/nccl` | **2.29.3** | `ncclProfiler_v1` … `v6` |
| du04 / `a100` | HPC SDK 25.9, `comm_libs/13.0/nccl` | — | `ncclProfiler_v1` … **`v4`** |

The first measurement read du04's `/lib/x86_64-linux-gnu/libnccl.so.2` — 2.28.9,
v1…v5 — and concluded v5. That is the **system** library and it is not what the
cases load; `$NCCL_HOME` points into the HPC SDK, whose NCCL is older. A v5
plugin would have exported a symbol du04's NCCL never looks for, and the failure
mode is silence: NCCL logs "is unsupported" at INFO level and runs on, leaving an
empty table that reads as a workload which did no collectives.

NCCL tries newest-first — `reference/nccl-git/src/plugin/profiler.cc:73-89` calls
`getNcclProfiler_v6`, then v5, v4, v3, v2, v1, taking the first that resolves.

### Therefore: export `ncclProfiler_v4` and only that

v4 is the newest version present on **both**: native on du04, reached through
NCCL's own down-shim on MN5. One struct, one code path, identical behaviour
everywhere we can run.

**What v4 costs, checked rather than assumed.** The down-shim
(`src/plugin/profiler/profiler_v4.cc:24-38`) forwards every `coll` field this
plugin reads — `seqNumber`, `func`, `count`, `root`, `datatype`, `nChannels`,
`nWarps`, `algo`, `proto` — and sets `parentObj` to the plan's group event.
`proxyOp` is forwarded whole. Nothing we read is dropped. What v4 genuinely
lacks is the `groupApi`/`collApi`/`p2pApi`/`kernelLaunch` families v5 added,
which this plugin does not enable.

**Do not be tempted by v3.** It looks attractive because its `coll` descriptor
carries `commHash` and `name` per event, which would solve the join for free.
On NCCL ≥ 2.27 those fields are dead: the down-shim sets them to zero and null
explicitly (`src/plugin/profiler/profiler_v3.cc:29-30`, `:47-48`, both commented
`// removed in v4`). A v3 plugin on 2.28/2.29 would produce rows naming no
communicator — unjoinable, which is the defect class this project exists to
avoid.

**The argument order differs between v4 and v5** and nothing catches it:
v4 is `init(context, mask, commName, commHash, ...)`, v5 is
`init(context, commId, mask, commName, ...)`. Both are pointers and scalars in an
order no compiler can check against a struct of function pointers.

## The join key, and its bounded gap

v4's `init` hands the plugin a `uint64_t commHash`. NCCL fills it from
`comm->commHash` (`src/plugin/profiler.cc:250`). That is **not** the
`comm_hash` on our existing `nccl_comm` record — ours is FNV-1a over the
`ncclUniqueId`, NCCL's is its own mixing function.

It is, however, exactly reproducible. For the ordinary init path,

```
comm->commHash = getHash(job->commId->internal, NCCL_UNIQUE_ID_BYTES)   // src/init.cc:1905
```

and `getHash` is a pure function of the bytes
(`src/include/bitops.h:502-543`: `eatHash` mixing into a two-word accumulator,
then `digestHash`). The interposer already sees the `ncclUniqueId` in
`ncclCommInitRank`. So we compute NCCL's own hash ourselves and the join is
deterministic — no thread-ordering assumption, no "the profiler init happens
inside the interposed call" hope. (It would be a hope, not a fact:
`ncclProfilerPluginInit` is called from `initTransportsRank`, which runs on an
async job thread when several comms are initialised inside a group.)

**The gap, stated rather than papered over.** Two init paths produce a
`commHash` that is *not* a function of any `ncclUniqueId` we can see:

- `ncclCommSplit` — `comm->commHash = digestHash(hacc)` over the parent chain
  (`src/init.cc:1877-1880`).
- the grow path — `hashCombine(baseMagic, nranks)` (`src/init.cc:1900`).

For those, `nccl_comm.nccl_comm_hash` must be **0**, meaning "not derivable",
and a `link_health` row must count profiler rows whose comm hash matches no
`nccl_comm`. A zero that says "unknown" is correct; a guessed value is not.

## One `.so`, not two

`NCCL_PROFILER_PLUGIN` is passed straight to `dlopen` when it contains a `/`
(`src/plugin/plugin_open.cc:79-95` — `openPluginLib` tries the literal string
first, and only mangles it into `libnccl-profiler-<name>.so` if it has no slash
and does not already look like a library name).

`libnccl_trace.so` is already `LD_PRELOAD`ed. Pointing `NCCL_PROFILER_PLUGIN` at
its absolute path makes NCCL `dlopen` the library that is **already resident**:
same handle, same process, same recorder, and the profiler callbacks can see the
interposer's communicator map directly. NCCL's `dlclose` on unload only drops the
refcount it took.

So this is **not** a new provider `.so`. It is a new door on the existing
`nccl_trace` provider, plus `ncclProfiler_v4` added to `nccl.map`.

## Volume is ours to choose

The plugin writes the activation mask (`int* eActivationMask`), so NCCL only
generates the event families we ask for. `ncclProfileProxyStep` fires per chunk
and would be enormous; we will start with

```
ncclProfileColl | ncclProfileP2p | ncclProfileProxyOp
```

and leave `ProxyStep`, `KernelCh` and `NetPlugin` behind a `PCOMM_NCCL_PROF_MASK`
override. Enabling them later is a one-line change; enabling them by default and
discovering the cost on a 16-rank job is not.

## Records

Three, all in `providers/nccl/nccl_records.hpp`, continuing the 900-block.

1. **`NcclProfComm`** (Artifact) — one per communicator, from `init`.
   `nccl_comm_hash`, interned `comm_name`, `nnodes`, `nranks`, `rank`.
   This is the bridge row: it is what `nccl_comm` joins to.

2. **`NcclCollExec`** (Event) — from the `coll` descriptor's start/stop pair.
   `nccl_comm_hash`, `event_id`, `parent_event_id`, `seq_number`, interned
   `func` / `algo` / `proto` / `datatype`, `count`, `root`, `n_channels`,
   `n_warps`. **The temporal is the execution interval NCCL reports**, which is
   what `nccl_collective` explicitly is not — that record's header says every
   temporal on it is an enqueue. These two must never be conflated.

3. **`NcclProxyOp`** (Event) — from the `proxyOp` descriptor.
   `event_id`, `parent_event_id`, `channel_id`, `peer`, `n_steps`, `chunk_size`,
   `is_send`, `pid`. This is the row that makes a collective's decomposition into
   per-peer traffic a recorded fact.

`event_id` is a per-process monotonic counter we assign; `parent_event_id` is
read out of the handle `parentObj` points at, which is a handle we minted. A
parent we never saw (because its family was masked off) records **0**, not a
fabricated ancestor.

## What this door still cannot see

Worth writing down before any of it is built, because a reader will otherwise
assume the opposite:

- **Device-initiated traffic.** GIN/IBGDA puts issued from inside a kernel do
  not go through the CPU proxy, so no `proxyOp` describes them. The `algo`/`proto`
  on the `coll` row is still true; the decomposition is not there.
- **NVLink / P2P-direct movement.** Same reason — no proxy op.
- **The bytes themselves.** `chunkSize × nSteps` is what NCCL *planned*, not what
  a completion confirmed. Joining that to `ib_post_send` volume is a separate
  question and must not be assumed to agree.

## Verification

Correctness first, so the order is: prove it locally, then prove it on hardware,
and do not let a green board stand in for either.

1. **`getHash` parity test** (local ctest). Reimplement `getHash` and check it
   against vectors taken from NCCL's own source. This is a pure function; there
   is no excuse for it to be wrong in a way we discover on a cluster.
2. **A fixture that can express the defect.** The `link_health` row for
   unmatched comm hashes needs a fixture where a profiler row deliberately does
   *not* match, or the assertion is vacuous. Same discipline as
   `every_rank_says_what_it_was_asked_to_do.sql`.
3. **Hardware, multi-node, unpinned.** Case 02 pins `NCCL_ALGO=Ring
   NCCL_PROTO=Simple` for byte-repeatability, which makes it exactly the wrong
   case to prove this door with — it would report back the value we set. A new
   case (or a variant) must run **unpinned** so the recorded `algo`/`proto` is
   NCCL's decision, and the assertion must be that the row is present and
   plausible, not that it equals a constant.
4. **The cross-check that matters.** On a pinned run, the recorded `algo`/`proto`
   must equal what `Invocation` says was requested. That is the one place these
   two records can be made to contradict each other, so it is the one place worth
   asserting equality.

## Order of work

1. ✅ `getHash` + parity test — no hardware needed. `tests/nccl_comm_hash_test.cpp`,
   goldens generated from NCCL's own `bitops.h`.
2. ✅ `nccl_comm_hash` column on `NcclComm`, 0 where not derivable.
3. ✅ The v4 plugin surface, mask, and the three records.
4. ✅ `nccl.map` export; `NCCL_PROFILER_PLUGIN` wiring in `suite/common.sh`.
5. ✅ Derived layer: `nccl_execution` and `nccl_peer_traffic`, plus three
   `link_health` rows.
6. ✅ du04 build + smoke. Case 20 PASS, 8 assertions. `proxyOp` empty as
   predicted (single node, no CPU proxy) and that did not read as failure.
7. ⏳ MN5 multi-node. Job 45350397 was RED on case 20 and that is the most
   valuable single result of this work — see below.
8. ✅ `catalog.md`, `BRIEF-BLACKBOX.md`, `gen_figures.py` counts.

## What the runs actually settled

**The join key, proven rather than argued.** On du04 case 20,
`nccl_comm.nccl_comm_hash` — computed by us from the `ncclUniqueId` — came back
`4313309560565514636`, byte-identical to the value NCCL handed the plugin and
recorded in `nccl_prof_comm`. Two independent computations of the same number in
different code at different times.

**`root` is NCCL's convention and not ours.** Every rootless collective came back
`root = 0`, where `nccl_collective` deliberately writes `-1` because 0 is a valid
rank. The two records must not be compared on that column, and the schema now
says so.

**`algo`/`proto` are uppercased.** `RING`, `SIMPLE` — while `NCCL_ALGO` is
spelled `Ring`. An assertion comparing them raw fails on a correct capture; the
one in case 20 folds case.

## What the first MN5 board found: NCCL cannot see its own broadcasts

Job 45350397: 21 of 22 cases PASS, case 20 RED, and the assertion that failed
was the one written two hours earlier — `24 exec vs 32 calls`. The 8 missing
collectives were **every single broadcast**, on all four ranks.

It is not the plugin. `NCCL_ALLGATHERV_ENABLE` defaults to **1**
(`src/enqueue.cc:30`), and with it `ncclBroadcast` is rewritten to
`ncclFuncAllGatherV` and enqueued on `planner->peers[root].bcastQueue`
(`enqueue.cc:2703-2722`). `ncclProfilerStartTaskEvents` walks `collTaskQueue`
and `p2pTaskQueue` and nothing else (`src/plugin/profiler.cc:478,520`). **No
plugin at any version is ever offered the event.** du04's older NCCL has no such
path, which is why it recorded all four operations and the gap only appeared on
2.29.

The first hypothesis was the v6 copy-engine event family, and it was wrong:
`ncclCeImplemented` (`src/ce_coll.cc:93`) lists AllGather, AlltoAll, Scatter and
Gather — not Broadcast — and AllGather was fully present. Checking beat guessing
by about ten minutes.

**The response is not to weaken the assertion and not to leave the board red.**
A permanently red board is a board nobody reads; deleting the clause gives back
the only check that catches a door dropping work. So the count comparison now
excludes broadcasts, a second clause fires if broadcasts are *partially* visible
(which matches no known NCCL path), and the broadcasts themselves are counted in
`link_health` as `nccl_bcast_invisible_to_profiler` — a note, because the tool
did everything available to it and `nccl_collective` still describes the call.

One trap found while fixing it: `nccl_collective.op` is the **name** in the CSV
export and the **enum** in the raw Parquet the derived layer mounts. Writing the
Parquet form in a CSV assertion reported "8 of 0" — a filter matching nothing,
dressed as a finding.

## The green board that was still wrong

Job 45350841 was SUITE GREEN — and reading the multi-node NCCL data underneath
it found two defects no assertion covered.

**Proxy ops named the wrong collective.** On case 02, proxy op 42's parent came
back as 42 — itself — and ops 43 and 44 also named 42. NCCL routinely hands back
a `parentObj` for an event it has already stopped, and the pool had recycled
that slot to proxy op 42. A refcount cannot fix this and NVIDIA's example plugin
has the same hole. The handle is no longer a pointer: it is (slot index,
generation) packed into the `void*`, slots live in blocks that are never freed,
and a generation is stamped on every acquire. A stale handle carries the old
generation and can never match a re-issued slot, so the answer is 0 — "not
observed" — instead of a plausible wrong id. The refcount is gone entirely,
which removes the ABA problem rather than defending against it.

**The rank layout was dropped whenever the interposer was not loaded.** NCCL
loads the plugin through `NCCL_PROFILER_PLUGIN` independently of whether pcomm's
nccl INTERPOSER provider was selected, so case 02 — `pcomm run -p ib` — has
912/914/916 records and no `nccl_comm` rows at all. `nccl_execution` read the
layout only from `nccl_comm` and returned NULL for all 160 rows, and
`link_health` called every one of them a defect, on a capture with nothing wrong
with it. `nccl_prof_comm` had rank, nranks and nnodes the whole time, told to us
by NCCL at init. The table now prefers it, and the defect row fires only when a
communicator is in NEITHER table.

**Open question, deliberately not answered here.** `suite/common.sh` sets
`NCCL_PROFILER_PLUGIN` unconditionally, so a run that asked for `-p ib` also
pays for NCCL's profiler callbacks. The data is well-formed and useful, but it
is a side effect the caller did not request. The right place for that decision
is the launcher, which knows the `-p` list; this plan does not make it.

## The lifetime bug, which no test would have found

`prof_stop_event` freed its `Event` with `delete`. That is wrong, and the reason
is in NCCL's source rather than in any capture:

```
eDescr.parentObj = sub->taskEventHandle;   // profiler.cc:574 — the COLLECTIVE
```

`ncclProfilerStartProxyOpEvent` takes `ncclProxyArgs` and runs on the **proxy
thread**, while the collective it names as parent is stopped from the
application thread. A child can begin, and dereference its parent to read the
id, after that parent has been freed.

NVIDIA's own example plugin atomically increments `parent->refCount` when a child
starts (`plugins/profiler/example/plugin.cc:531`). That is as direct a statement
as one gets that the lifetime is not guaranteed.

Events now come from a pool whose slots are never returned to the allocator — so
a pointer NCCL kept is always readable memory — recycled only under a refcount,
with a generation packed beside it to close the ABA window. A slot that was
freed and re-acquired fails the compare-exchange and the child records parent 0,
"not observed", rather than naming whichever event occupies the slot now.

**No test on any machine we have would have caught this.** It needs a proxy op
and a collective racing on two threads, which is a multi-node NCCL run, and even
there it would corrupt silently far more often than it would crash. It was found
by checking the lifetime instead of assuming it — the same move that rejected a
thread-local for the commHash join.

## Three defects this work surfaced

1. **The first version targeted profiler v5**, chosen from du04's *system*
   libnccl. The cases load `$NCCL_HOME`, a different and older NCCL. Caught by
   checking the symbol table of the library the cases actually load rather than
   the one on the default path.
2. **`link::columns_match` swallowed SQL errors.** Its comment said "the CREATE
   already reported why" — true of `kViews`, false of `kTables`, which are never
   CREATEd. `pcomm link` printed "(SOME FAILED, see above)" with nothing above
   it. Fixed; it now prints the planner error.
3. **`run_on_a100.sh` built through `cmake --build | tail -3`**, so a failed
   build exited 0 and the runner proceeded against the previous binary. Finding
   14 verbatim, still present in this file after being fixed elsewhere.

## Two vacuous passes caught, both by measuring

**The assertion.** `nccl_stated_what_it_chose.sql` first declared
`-- requires: nccl_coll_exec`. Re-running case 20 with
`NCCL_PROFILER_PLUGIN=none` reported `[skip]` and `PASS` over a capture that had
lost the entire profiler layer. With the requirement dropped it fails, naming
every rank.

**The fixture.** `tests/nccl_execution_rule.sh` passed on its first run. Deleting
`AND x.nccl_comm_hash <> 0` from the join — the clause that stops every
underivable communicator marrying every other one — changed nothing, because no
fixture row had a zero hash on the execution side and the clause was unreachable.
Adding that row makes the mutation fail three assertions. The rank-scope mutation
on the parent join was caught from the start (`4|2` where `2|1` is right).

## The side effect this left behind, and what it measures out at

`suite/common.sh` sets `NCCL_PROFILER_PLUGIN` whenever the library is built and
the variable is unset — for **every** case, not only the ones that select the
nccl provider. That was flagged when finding 9 closed as a side effect the
caller did not request, and it is what produced case 02's odd shape: 8
`nccl_prof_comm`, 160 `nccl_coll_exec` and 320 `nccl_proxy_op` rows beside
**zero** `nccl_comm` and `nccl_collective` rows, in a capture that asked for
`-p ib`.

Put to the user on 2026-09-03. The answer was that it is fine **provided it does
not alter the behaviour of runs that did not ask for `-p nccl`**, so that became
the thing to measure rather than to argue. Measured on du04, same workload, same
`-p ib`, twice, the only difference being the variable:

| | plugin off | plugin on |
|---|---|---|
| workload's own result | `ranks=4 count=1048576 iters=8 ok` | identical |
| NCCL's algo/proto/channel picks | 121 distinct patterns | **identical** |
| wall clock, 4 interleaved reps | 3.66–3.69 s | 3.63–3.68 s |
| `ib_device`/`ib_port`/`invocation`/`node_identity` | 1/1/1/1 | unchanged |
| `intern_string`, `intern_text` | 6 | **13** |
| `nccl_coll_exec`, `nccl_prof_comm` | absent | 64, 4 |

And for an application that never calls NCCL at all — cases 01, 08, 09, 19, 21,
22, most of the suite — it is entirely inert: case 09 run twice produced **22
comparable streams with byte-identical sizes** and no 9xx stream either way.
NCCL loads a profiler plugin from inside `ncclCommInitRank`, so a process that
never builds a communicator never reads the variable.

So: the RUN is untouched; the CAPTURE gains two tables and seven dictionary
entries, and only when the application actually uses NCCL. The timing difference
is noise and says so by having the wrong sign — the plugin-on runs came out
marginally faster.

**One hypothesis died here, and it was the only one worth having.** Plugin
records go through the same thread buffers and the same arena, and this tree
already knows that `invocation` "rides the arena-flush path, which needs a
record buffer to FILL — so a short capture never reached it" (measured on job
45344306, where three cases carried no `invocation` row). More records means
more flushes, so the plugin could have made rows appear in captures that have
nothing to do with NCCL. It did not: `invocation` is 1 either way.

**Three vacuous results were caught getting here**, each of which would have read
as a clean pass: the "workload output" diff was really diffing NCCL log lines
containing pids; the algo comparison compared two *empty* greps and printed
"identical: YES"; and the first inert-plugin run compared two captures holding
*zero* streams. The measurement script now refuses each of those explicitly.

**Known thin spot.** In the NCCL-using half the `-p ib` capture held only 2 ib
rows, so "ib rows unchanged" rests on very little there. The strong evidence is
the 22-stream byte-identical comparison, which covers the non-NCCL case only. A
run of case 02 with the plugin deliberately off would close it; nothing on MN5
currently produces that counterpart.
