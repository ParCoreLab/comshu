# PLAN — instrumenting a multi-launch workload poisons the CUDA context

- **Created:** 2026-08-10 08:54 +03
- **Status:** ROOT CAUSE OPEN. Both teardown symptoms are fixed and shipped; the
  fault that causes them is not.
- **Blocks:** cases 13/14/15 (all red), and p2p as a UI sample capture.

## The finding, in one line

A workload that runs cleanly with no profiler raises an **illegal memory access**
when captured with `-p p2p`, and the fault is charged to the instrumented kernel
launch, not to teardown.

## Evidence already in hand

Do not re-derive these. Each cost a queue slot.

**The workload is not at fault.** Isolation probe, MN5 job 44458142, one node
(as07r4b32), same binary in both sections:

| section | command | result |
|---|---|---|
| A | `asym_patterns hub 4096 3`, bare, 4 PEs | **exit 0**, all four PEs print correct edge counts |
| B | same binary under `pcomm -p p2p` | illegal memory access, then hang |

**The object registry is not implicated.** gpumem's heap and peer registration is
byte-identical between the failing cases and the passing case 04, on every round:
`1 of 4 regions are my heap, 4 peer ranges registered`, then `0 of 98`, then
`0 of 194`. Identical strings, identical order.

**The fault follows kernel launches, exactly.** From job 44457953, counting ranks
that hit the assertion:

| case | ranks that faulted | ranks that launch ≥1 instrumented kernel |
|---|---|---|
| 13_hubspoke | 4 | 4 — PE 0 launches 3, spokes 1 each |
| 14_pipeline | **3** | **3** — PE 3 sends nothing, so launches nothing |
| 15_skew | 4 | 4 — every PE has an outgoing edge |

`pipe`'s PE 3 is an accidental control: the one rank in the whole set that
launches no instrumented kernel is the one rank that does not fault. This is the
strongest single piece of evidence and it was free.

**`14_pipeline` already refutes the obvious hypotheses.** Its per-PE shape is the
same as case 04's, which passes: one kernel launch per PE per iteration, to one
peer. It still faults. So the cause is NOT "several launches per iteration", NOT
"several peers per PE", and NOT "varying grid size between launches within a
rank" — all three are absent from pipe.

**Sizes are not it either.** Case 04 pushes 65536 elements per launch; these push
at most 4096.

## What remains different from case 04

After pipe eliminates the launch-pattern hypotheses, the surviving differences
between `put_slice` (faults) and case 04's `put_to_next` (passes) are small:

```
  put_to_next(int* dst, const int* src, int  n,              int peer)
  put_slice  (int* dst, const int* src, long n, int peer, long dst_off)
```

1. **64-bit index arithmetic.** `long i`, `long n`, and a store address computed
   as `&dst[dst_off + i]`. put_to_next indexes with `int` and no offset.
2. **A non-zero base offset into the target buffer.** put_to_next always writes
   from element 0; put_slice writes at `dst_off = mype * slot`.
3. **Grid size differs per rank** (pipe sends `slot*(mype+1)/npes`), where case
   04 launches an identical grid on every rank.
4. **Three symmetric buffers** rather than two.

(1) and (2) are the prime suspects, because they change the SASS of the
instrumented store — which is precisely what NVBit rewrites.

## Already shipped — do not redo

- **f34fbb3** — `nvbit_at_ctx_term` no longer `abort()`s on a failed flush. It
  consumes any error latched before us (cudaGetLastError is per-thread, so
  asserting on it charged pcomm for the application's errors) and reports rather
  than killing the profiled process at exit.
- **a3d7569** — a poisoned context no longer hangs teardown. The recv thread's
  only exit was the flush token that the terminating kernel writes; when that
  kernel cannot run, `pthread_join` blocked forever. `CtxState::recv_abort` gives
  it a second exit, checked before each `recv`.

Together these turn "SIGABRT, capture lost" and "hung until walltime" into "one
error line, partial capture written". **They do not fix anything.** They stop the
bug costing an allocation per attempt.

## Steps

### Step 1 — compute-sanitizer, for the faulting kernel and address

The decisive measurement, and it should come first because every later step is
cheaper once it is known.

```
compute-sanitizer --tool memcheck --log-file cs.%q{SLURM_PROCID}.log \
  pcomm -p p2p -o $OUT -- asym_patterns pipe 4096 3
```

Run `pipe`, not `hub`: it faults on 3 of 4 ranks with the simplest possible
launch shape, and rank 3 gives a clean in-run control.

Two cautions. NVBit and compute-sanitizer both rewrite SASS and have been known
to interact; if the run will not start, fall back to `--tool memcheck` with
`PCOMM_KERNEL_NAME` unset so nothing is instrumented, to confirm the sanitizer
itself is clean on this workload. And expect it to be slow — budget the full
2-hour `acc_debug` wall, one node, 4 PEs.

**Decides:** whether the bad address is inside the instrumented store (our SASS
rewrite), inside the channel push (`inject_funcs.cu`), or in NVSHMEM code.

### Step 2 — bisect the kernel, one difference at a time

Only if step 1 is inconclusive. Build variants of `put_slice` and run each under
`-p p2p`, changing exactly one thing:

| variant | change | refutes if it passes |
|---|---|---|
| A | `long` → `int` for `i` and `n` | 64-bit index arithmetic |
| B | drop `dst_off` (always write from 0) | non-zero base offset |
| C | fixed grid on every rank | per-rank grid variation |
| D | one symmetric buffer instead of three | buffer count |

Each is a two-line edit to `tests/hpc/asym_patterns.cu`. Run them in ONE job —
the QoS allows one job at a time and eight nodes, so a single allocation running
four variants sequentially is four times cheaper than four submissions.

### Step 3 — fix, then prove the fix on the control

Whatever the cause, the regression test already exists: cases 13/14/15 go green,
and `14_pipeline`'s PE 3 must still record zero accesses (`last_rank_silent`).
A fix that makes the fault disappear by also suppressing instrumentation would
pass the first and fail the second, which is why that assertion is worth more
than it looks.

### Step 4 — decide what the guard should be

Independent of the root cause: `nvbit_at_ctx_term` currently discovers a poisoned
context at the very end, after all the records are already unrecoverable.
Consider checking `cudaGetLastError()` after each instrumented launch (behind an
env flag if it costs a sync), so the capture reports WHICH launch poisoned the
context instead of only that one did.

## Explicitly out of scope

- The `-p ib` / mlx5dv path. It is unaffected — this is an NVBit/gpumem fault —
  and the multi-node ibverbs variants of these workloads are tracked separately.
  p2p must not be used as a UI sample capture until this closes.
- The three workloads themselves. Section A proves them correct; do not "fix"
  them to work around the profiler.

## Risks

- **compute-sanitizer may not compose with NVBit.** If so, step 2's bisect
  becomes the primary route and gets more expensive. Mitigate by running the
  variants in one allocation.
- **The fault may be in vendored NVBit** (`nvbit-1.7.7.1`), not in our code, in
  which case the fix is a workaround in `inject_funcs.cu` or an instrumentation
  constraint we document. Decide that only with step 1's address in hand.
- **One-job QoS.** Every speculative submission costs the next real one. Batch.
