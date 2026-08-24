# CONTINUE_ME

Working note, 2026-08-17. Everything that was outstanding when this was written
has landed: **the suite is green, 15 PASS and 4 SKIP, with no case reading a
stale capture except the four du04-only IBGDA ones.**

What is left here is §5 (known-blocked, do not re-derive), §6 (environment
gotchas) and §7 (mistakes), which are reference rather than a to-do list, plus
the one new finding in §2 about NCCL accesses landing outside every tracked
object. Move those somewhere permanent and delete this file.

HEAD when written: `255c276`. Since then, on `main` and NOT yet pushed to
`origin`/`mn5`: `cd9e29e` (16-18 to acc_debug), `4bdd49e` (UI SO_REUSEPORT),
and the ucx teardown fix.

---

## 1. DONE: cases 16-18 on MN5 — all three PASS, 12 checks each

`tests/hpc/run_on_mn5.sh --cases "16 17 18" --per-case`, launched from a
background shell. **`--per-case` is not optional** — 16-18 are 4 nodes x 1 GPU
and the combined allocation gives them the wrong geometry, which reads as a
tracer regression (empty captures, `tables_present` FAIL). See
[[pcomm-mn5-combined-alloc-breaks-multinode-cases]].

16 (`44714819`) and 17 (`44714918`) PASS. 18 was job `44715122`, which spent 80
minutes PENDING on `acc_ehpc` and was cancelled and resubmitted to `acc_debug`
as `44718913`: started in 18 s, ran in 32 s, **PASS, 12 checks**. All three
green. See §6 for the QoS reasoning, now baked into their `job.sbatch` headers.

Note if you resubmit a case by hand: the manifest at
`$PCOMM_OUT_ROOT/.last-run.manifest` still names the cancelled job id, and
`run_suite.sh check` reads it. Repoint the line or the board reports the old
capture.

Log: `.../scratchpad/mn5-run4.log` (scratchpad is session-scoped and will be
gone — re-run rather than hunt for it).

**What to check when it lands.** These three are the only cases carrying the
`gpus` meta fix (`854b785`), and they have no `gpu_access`, so the schema change
in `793de4b` cannot touch them. Expect PASS, 12 assertions each. If they are
red, suspect the run geometry before suspecting the tracer.

## 2. DONE: cases 05 and 19 — both PASS on job `44719051`

`acc_debug`, queue wait **23 s**, in-job 05 = 481 s / 19 = 42 s. Run with:

```bash
PCOMM_REMOTE=koc296797@alogin2.bsc.es tests/hpc/run_on_mn5.sh --cases "05 19"
```

(`alogin1` refuses SSH often enough to have cost two runs; alogin2 is the same
home directory.)

### 05_nccl_p2p — PASS, 9 checks, and no bus error

Both open questions answered, and the red row is confirmed as the artefact it
was believed to be. `mem_object` is **1 per rank**, the golden value, against
the 8 per rank the reverted `cuMemCreate` probe (`a05aa28`, job `44699869`)
produced by recording CUDA's 2 MiB pool chunks as application objects.

The bus error did NOT recur. That is one clean run against one failure
(`44698048`), so "flaky" is now the better reading than "persistent" — but a
single pass does not close it. Do not treat it as fixed; nothing was fixed.

**NEW FINDING, and the interesting one.** The stride 80 -> 88 change did what
it was supposed to: this case now records **1,179,948 `gpu_access` rows** where
the old capture read 0. And **not one of them lands in a tracked object** —
`object_access` is empty and `link_health.unattributed_p2p_accesses` is the
whole 1,179,948.

It is not an attribution regression. The same pipeline places 65,536/65,536 on
case 03 and 1,573,532 on case 07. It is specific to NCCL, and the addresses say
why: on every rank the accesses sit in a band starting exactly **14 MiB above**
the tracked object's base and ending well past its 1 MiB extent —

| rank | accesses | inside tracked object | access lo | object base |
|---|---|---|---|---|
| 0 | 294987 | **0** | 140089000722432 | 140088986042368 |
| 1 | 294987 | **0** | 139941361221632 | 139941346541568 |
| 2 | 294987 | **0** | 140155572715520 | 140155558035456 |
| 3 | 294987 | **0** | 140064740868096 | 140064726188032 |

NCCL's LL protocol spins on its own internal channel buffers, not on the user
buffer, and those come from an allocation door gpumem does not record. Which is
exactly what the reverted `cuMemCreate` probe was reaching for — so that
experiment was aimed at a real gap and missed by recording pool chunks instead
of application objects. **This is a design question, not a bug to go fix**: the
Objects panel cannot show NCCL traffic until there is a way to name an
allocation that the application never made. Case 05's assertions are unaffected
— they check `gpu_access` structure directly (one allocation per rank, both
directions, peer traffic), which is why 9 checks pass over an empty
`object_access`.

### 19_ucx_mpi — PASS, 6 checks, and the provider half verified on hardware

The note below said not to accept a green board here. It was checked directly,
on `ucx_mpi-44719051`:

- The duplicate-close query returns **nothing**. It was 8 rows of "more closes
  than opens (opener 604)".
- `entity_close` for opener 604 is **5 per rank**, against **5 `ucx_endpoint`
  opens per rank**. Exactly matched on both ranks, where one teardown used to
  emit 2-3 rows.

The fix did not go too far, and that is by construction rather than by luck:
`close_endpoint_once` suppresses only a handle it has already seen closed.
Absent -> emit, open -> emit, closed -> drop, and a create on the same address
resets to open. So an endpoint created through a door we do not hook still
emits its unmatched close — the one signal the assertion exists to deliver —
and pointer reuse after a genuine close still records both closes.

One thing was fixed on the way through, precautionary, in this session: that
per-handle map was a namespace-scope `std::unordered_map` on a path reachable
from process teardown (this file registers its own
`__attribute__((destructor))`, and endpoints can be closed from an atexit
handler). Same shape as the gpumem segfault in §7. It is now immortal — see the
comment in `ucx_provider.cpp`. No crash was ever observed; 44719051 passed with
the old code.

## 2b. DONE: the NCCL finding above has a provider now

`providers/nccl` — the host API, through NCCL's PROFAPI door. Case 20
(`20_nccl_collectives`) is **PASS, 8 checks, 14 s** on job `44724132`, and the
suite is 16 PASS / 4 SKIP.

It does not attribute the 1.18M accesses and was never going to; it records the
layer where NCCL's communication is actually stated. Per rank: 1 `nccl_comm`,
8 `nccl_collective`, 4 `nccl_p2p`, 4 `nccl_group`, 1 close. The cross-rank join
is confirmed on hardware — all four ranks carry one `comm_hash` with distinct
`nccl_rank`.

Three defects found by three 30 s jobs, all mechanical, none in the record
design:

1. `PCOMM_PROVIDER_PATH` in `common.sh` lists provider dirs by hand and the new
   one was missing. The launcher refused loudly rather than running untraced.
2. The hooks resolved NCCL's `pnccl*` twin while exporting `ncclX`.
   `register_real` learns its own base by `dladdr`-ing an RTLD_DEFAULT lookup of
   `Trait::symbol_name`, so `our_base` became LIBNCCL's base and every correct
   resolution was discarded as self-resolution. **A hook must resolve the name
   it exports.**
3. A bare `HAVING` over an un-grouped `SELECT` does not parse in sql_check's
   engine, and an assertion that cannot parse looks exactly like one that found
   nothing. Worth remembering the next time an assertion is suspiciously quiet.

### Next: GPU-initiated NCCL, and it is NOT blocked

The device API is entirely `NCCL_DEVICE_INLINE` — inlined into the application's
kernel, no symbol to interpose — so the capture route is NVBit, which makes it a
p2p-shaped case rather than a host-API one. The LSA path (`ncclGetLsaPointer`)
is plain peer loads and stores that gpumem already sees, and it will ATTRIBUTE
where case 05 cannot: the buffers arrive through `ncclCommWindowRegister`, which
the nccl provider hooks, so `nccl_window` names them. The GIN path is
IBGDA-shaped and inherits the `PeerMappingOverride` blocker.

No NCCL build is needed — I was wrong about that earlier and cancelled the job.
`/apps/ACC/NCCL/*` tops out at 2.24.3 with no device API, but the cases resolve
`NCCL_HOME` from the HPC SDK (`common.sh:47`), and SDK 26.5 ships **2.29.3 with
`nccl_device.h`**. Starting point:
`reference/nccl-git/docs/examples/06_device_api/01_allreduce_lsa/c/main.cu`.

**Check first**: the runtime library and the headers may not be the same NCCL.
`NCCL_HOME` globs to 26.5 while `module load nvidia-hpc-sdk/25.7` puts 2.26.5 on
`LD_LIBRARY_PATH` — the case sbatch prepends `$NCCL_HOME/lib`, so it should win,
but 2.26.5 has NO device API and a case that silently ran against it would fail
in a confusing way. `nccl_window`/`nccl_dev_comm` staying empty is the symptom.

## 3. DONE: the `SO_REUSEPORT` footgun

Fixed in `pcomm-ui/main.cpp` — `srv.set_socket_options` now sets `SO_REUSEADDR`
only. Verified by hand: two instances on `127.0.0.1:8791`, the second exits 1
with "cannot bind" and names `ss -ltnp` instead of silently sharing the port.
Restart-after-Ctrl-C still works, which is what `SO_REUSEADDR` is there for.

The original report, for the record:

`pcomm ui` uses cpp-httplib, which sets `SO_REUSEPORT` by default. A second
instance on the same `addr:port` therefore **shares** the port instead of failing
with EADDRINUSE, and the kernel round-robins requests between them.

Observed today, not theorised: a 6.5-hour-old UI serving the pre-schema
`hubspoke-44465477` was squatting on `127.0.0.1:8777` beside a fresh one on
`nvshmem_multinode-44714285`. Identical queries returned different answers at
random — `nvlink 6` (hubspoke) or `verbs 45 + nvlink 6` (multinode). A demo
could silently show last week's capture.

Fix: `srv.set_socket_options(...)` in `pcomm-ui/main.cpp` omitting
`SO_REUSEPORT`, so the second bind fails loudly. Around line 335, at
`srv.listen(bind_addr.c_str(), port)`.

---

## 4. Live processes (will not survive a reboot)

| what | address | notes |
|---|---|---|
| `pcomm ui` | `100.64.0.3:8777` | tailnet, panels only |
| `pcomm ui` | `127.0.0.1:8777` | loopback, for the agent path |
| `agent/bridge.mjs` | `127.0.0.1:8778` | loopback only, by design |

Serving `~/waste/pcomm/corpus/nvshmem_multinode-44714285` — chosen because it is
the only fresh capture populating every panel: 8 ranks over 2 nodes, 2.53M rows,
7 timeline lanes, and both transports in one comm matrix (45 verbs + 6 nvlink).

Full stack on this box:
`http://127.0.0.1:8777/?agent=http://127.0.0.1:8778`

From another tailnet device:
```bash
ssh -L 8777:127.0.0.1:8777 -L 8778:127.0.0.1:8778 mktips@100.64.0.3
# then http://127.0.0.1:8777/?agent=http://127.0.0.1:8778
```

The agent **cannot** be made tailnet-visible without editing two deliberate
guards in `bridge.mjs`: it refuses any non-loopback `--bind` ("no
authentication ... use an SSH forward"), and its CORS allowlist admits only
`http://127.0.0.1:<uiPort>` / `localhost`. Hence the second loopback UI rather
than a weakened guard. That is a decision for a human.

Model: `muse-glimmer-30b` via `simurgh-base:10000` (pi provider `simurgh`).
Verified answering, and its numbers verified against the capture.

---

## 5. Known-blocked, do not re-derive

- **p2p object offsets.** Every p2p `object_access` band starts at 0, so the hub
  heatmap shows three writers stacked at `[0, 16384)` where the ib twin shows
  correct bands. `PLAN-20260817-0705`, blocked on evidence: both routes probed
  and shut. Show the Objects panel from the `_ib` captures.
- **`source_line` is empty everywhere.** Needs `-lineinfo`; the sbatch files
  build with it now but no harvested capture predates that. Explicitly set aside
  for this pass.
- **The `gpu_access` window is a bound, not a measurement.** `kind = 3` in
  `timeline_span` says so. Do not render it as a duration.

## 6. Environment gotchas that have each cost a run

- `alogin1` refuses SSH; use `PCOMM_REMOTE=koc296797@alogin2.bsc.es`.
- One job at a time **in `acc_debug`** (`MaxJobsPU=1`, `DenyOnLimit`). This is a
  per-QoS limit, not a per-user one — `acc_ehpc` allows 366, so a debug job and
  an ehpc job run concurrently. Runs still block; launch them in the background.
- `squeue`'s `START_TIME` for a PENDING job is not an estimate. On job 44715122
  it read "now + 2 s" on every poll for 80 minutes. Trust `Reason=` and `sprio`,
  not that column.
- **A long PENDING on `acc_ehpc` is worth a resubmit to `acc_debug`** whenever
  the job fits (node<=8, walltime<=2 h). The QoS priority weight is 10000 vs
  100: job 44715122 sat 80 min at priority 30830; the same job as 44718913 was
  129303 and started in 18 s. Cases 16-18 are `acc_debug` as of this commit.
- The gate refuses a dirty tree (it pushes HEAD to the bare repo). Commit first,
  or `--rsync` while iterating.
- Boards print rows for cases the run did not touch. Since `255c276` those say
  `[stale: not in this run]` — read that before believing a red square.
- `nvbit_backend.cu` and `pcomm-read/main.cpp` had pre-existing clang-format
  violations outside my hunks; the tree is fully formatted as of `381e67e`, so
  the pre-commit hook now enforces it.

## 7. Mistakes made today, so they are not repeated

- Replaced a POD `static` with a `std::unordered_map` inside a path that runs
  from an **exit handler**. Its destructor had already run; every gpumem case
  segfaulted at address `0x8`. `nvbit_at_term` states the rule two functions
  below: "statics may be gone". State touched during teardown must be trivially
  destructible — there is now a `static_assert` for it.
- Trusted a green board. Case 03 passed for four runs while the code under it
  had never executed, because the flag that gated it answered a different
  question.
- Used `git add -A` and swept the `run_suite.sh` board fix into `255c276`, whose
  message does not mention it. The reasoning is in the code comment instead.
