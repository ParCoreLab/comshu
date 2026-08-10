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

## CORRECTION, 2026-08-10 — the case 04 comparison is not controlled

Everything below this heading supersedes the original suspect list, which was
built on a comparison that does not hold.

**Case 04's binary is cached and case 13/14/15's is not.** `04_nvshmem/job.sbatch`
guards its build with `[ ! -x "$APP" ]`, so it has never been rebuilt; its
statically linked `libnvshmem_device.a` is whatever SDK was newest when it was
first built. The new cases rebuild against `NVSHMEM_HOME`, which `common.sh`
resolves by globbing and taking the NEWEST (26.5 on MN5) while
`pcomm_load_runtime` loads module 25.7. Case 16's own sbatch already says this in
another context: "Case 04 does not hit this only because its binary is cached
from before the two diverged."

So "same per-PE shape, one faults, therefore the cause is in the remaining kernel
differences" does not follow. The two binaries carry different NVSHMEM device
libraries.

**And the differences the plan listed are the wrong order of magnitude.** The
SASS NVBit rewrites inside `put_slice` is overwhelmingly NVSHMEM's inlined
`nvshmem_int_p`, not the four lines of the kernel body. A different NVSHMEM major
version means a different instruction mix in exactly the code that has already
broken under instrumentation once — see below. `long` vs `int` indexing is a
rounding error beside that.

**The PE 3 control proves less than claimed.** `PCOMM_KERNEL_NAME=put_slice`
means instrumented code exists ONLY inside that kernel, so a rank that launches
nothing executes no instrumented SASS. The control establishes that executing
instrumented code is necessary — already known from case 07, where NVBit with a
filter matching nothing runs fine. It does NOT localise the fault to
`put_slice`'s own body, and treating it as if it did is how the suspect list came
to contain only the kernel signature.

**Pre-refuted, recorded so it is not re-derived:** partial-warp tails do not
correlate. `hub` (16 and 2 blocks), `pipe` (4/8/12) and case 04 (256) are all
exact multiples of 256; only `skew` produces a partial block. All three fault,
only one diverges.

## The prior art, corrected

The earlier NVBit hang was fixed by **`3c5d55f`**, "gpumem: fix the case 07 hang
— only instrument global memory". It changed a memory-space blocklist into an
allowlist (`nvbit_backend.cu:380-386`): instrument GLOBAL, GENERIC and
GLOBAL_TO_SHARED only. Its bisect, via the `PCOMM_INSTR_LO/HI` harness that still
exists, found `[0,1006)` completing in 22 s and `[0,1007)` hanging, where index
1006 is an `LDL.64` — a LOCAL access, per-thread stack — inside
`nvshmemi_transfer_rma_p<int>`. The 64-bit global atomic and the STRONG.SYS
stores on either side were innocent.

Two things this corrects in the recollection that prompted the investigation:

- The filter that fixed it is on ADDRESS SPACE (local/shared vs global), not on
  `run_device == mem_device`. Those are different filters and only the first ever
  fixed anything.
- `USE_ASYNC_STREAM` was a REFUTED theory, not part of the fix —
  `things-to-circle-back-to.md:424` lists it among six theories that "died on the
  way, each refuted by measurement": it "behaves identically". It is not defined
  in this build and is not plausibly implicated in an illegal access.

Also established: `RemoteFilter` runs HOST-side (`gpumem/engine.hpp:190-193`,
applied in `attr/pipeline.hpp` from `recv_thread_fun`), so the device pushes
every GLOBAL access regardless of locality — `mem_device` is not even known on
the device. That costs bandwidth, not safety: `pipe`'s heaviest rank pushes about
1 MB against a 1 GiB ring, so the fill/flush/wrap path never runs.

## Steps, revised

### Step 1 — equalise the libraries before bisecting anything

Rebuild **case 04's own `putget.cu`, unmodified**, against the current
`NVSHMEM_HOME`, and run it exactly as case 04 does (`putget 65536 3`,
`PCOMM_KERNEL_NAME=put_to_next`, `-p p2p`). Delete `$HOME/nvshmem_putget` first
so the build fires. This holds the workload at the known-passing one and changes
only which NVSHMEM the device code comes from.

- **Faults** → the kernel-signature differences are a ghost, the cause is the
  library/toolchain, and the four-variant bisect must not be run.
- **Passes** → the version is exonerated and one variable is gone.

### Step 2 — the instruction bisect, not compute-sanitizer

`PCOMM_INSTR_LO/HI` already exists (`nvbit_backend.cu:398-427`), indices are
stable because they are assigned before the range test, and `PCOMM_INSTR_DUMP`
turns an index back into opcode/function/offset. Each run is seconds at 4096x3,
so ~11 runs binary-search the whole instrumented set.

This replaces the original step 1. It is cheaper, it names the INSTRUCTION rather
than an address, it has already solved this exact class of bug once, and it
carries none of the "compute-sanitizer may not compose with NVBit" risk the
original plan flagged. Run it on `pipe`.

Both fit in one allocation. If a third section fits, build with
`-DPCOMM_INJECT_NO_PUSH` (`inject_funcs.cu:41-51`) to separate "the injected call
perturbs the kernel" from "push() interacts with it".

### Step 3 — fix, then prove it on the control

Unchanged: cases 13/14/15 go green AND `14_pipeline`'s PE 3 still records zero
accesses. A "fix" that works by suppressing instrumentation passes the first and
fails the second.

## Separate latent bug, found while reading — not the cause

`inject_funcs.cu:24-26` fills all 32 lanes with `__shfl_sync(active_mask, addr, i)`.
For a lane NOT in `active_mask` the shuffle result is undefined, not zero — but
`dev_common.h:16` documents `addrs[32]` as "0 = inactive lane" and
`nvbit_backend.cu:608` skips only exact zeros. Under any divergence at an
instrumented instruction this manufactures phantom addresses. `RemoteFilter`
drops most of them (garbage resolves to no allocation, so `mem_device` becomes
`run_device`), but garbage landing inside a peer range becomes a FABRICATED
remote access. Fix: zero `ma.addrs[i]` when lane `i` is not set in `active_mask`.

Worth doing regardless of this bug, and worth doing before trusting any capture
from a workload with divergent stores.

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
