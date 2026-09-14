# PLAN — the injected function is a tools patch, and case 11 is a du04 case

- **Created:** 2026-09-14 13:00 +03
- **Status:** done. `d2ff87c..b1e244a`. MN5 board 45747672 green (23 PASS / 2
  SKIP / 0 FAIL), du04 board green (09, 10, 11 all PASS). Three things remain
  open, all named at the bottom; none of them blocks anything.
- **Related:** the `FIXED (2026-09-09)` 64-bit-index entry in
  `things-to-circle-back-to.md` (the finding itself lives there, not here);
  `../nvbit-bug-report` (the standalone reproduction, synced through `~/pockt`);
  `PLAN-20260810-0102-sigmatrace-parity-done.md` (the comparison case 11 still
  owes).

## Why this file exists

Four decisions here are the kind that look arbitrary later and get undone by
someone reasonable. Two of them were *already* made wrongly once in this same
sequence of commits, confidently, with prose attached — so the rejected options
are written down with the measurement that killed each one.

## 1. `-Xptxas -astoolspatch` must reach the injected function's own compile

**Constraint, and it is load-bearing: no `__global__` may ever return to
`gpu-backends/nvbit/inject_funcs.cu`.**

ptxas refuses `--compile-as-tools-patch` in a translation unit containing an
entry function. `pcomm_trickery` was such a function, so the build could not
apply the flag at compile time and moved it to `$<DEVICE_LINK:...>`, where `-dc`
has already had ptxas generate the SASS. It was accepted, and inert. Without it
ptxas gave `pcomm_instrument_mem` uniform registers UR4–UR27, NVBit preserves
only UR4:UR5 across an injected call, and every kernel holding a live pointer in
UR6+ at an instrumented instruction had it destroyed.

Re-adding any entry function to that TU forces the flag back to the device link
and silently reverts the fix. There is no error; the only symptom is corrupted
captures on some kernels and not others.

**Verify rather than assume** — `providers/gpumem/CMakeLists.txt` carries the
one-liner. Any `UR` in `pcomm_instrument_mem` means the fix is not in effect:

```sh
cuobjdump -sass libgpumem_trace.so | awk '/Function : pcomm_instrument_mem/{f=1} \
  f&&/Function : /&&!/pcomm_instrument_mem/{f=0} f' | grep -oE 'UR[0-9]+' | sort -uV
```

**Rejected:**

- *Keep the flag on `$<DEVICE_LINK:>` and cap registers instead.* Measured:
  `-maxrregcount` swept 16, 24, 32, 48, 64, 255 gives UR4–UR27 at every cap. It
  is not register pressure.
- *Report it to NVIDIA.* NVBit ships the mechanism that makes this safe and uses
  it in its own sample Makefiles. Compiled as NVBit intends, an injected
  function has no uniform registers to save. Ours defeated the flag.
- *Blame the 64-bit index, `IMAD.WIDE`, the generic `ST.E`, `LDG.E.64.CONSTANT`,
  the CUDA version, or the injected function's shape.* All six refuted by
  experiment before the real cause was found; the table is in
  `things-to-circle-back-to.md`. The trap worth knowing: a faithful copy of the
  injected function — same eight arguments, same warp shuffles, same 32-iteration
  loop, same register cap — runs **clean**, because the repro's Makefile compiles
  it `-Xptxas -astoolspatch` and it therefore uses no uniform registers at all.
  It reproduces the shape and cannot reproduce the bug.

## 2. Case 11 captures with `-p ib,mlx5dv`, and the `ib` is not decoration

NVSHMEM's ibdevx transport creates queue pairs with plain verbs. `ibv_destroy_qp`
is interposed by the **engine** (always present); `ibv_create_qp` by the
**ibverbs provider**. A mlx5dv-only capture therefore records a close for every
queue pair and an open for none — 99 orphans per rank, measured both ways on one
node (job 45636769).

It earns its place a second time, which is the part to not undo: `ib_mem_region`
is an ibverbs-provider table, and that is where the GPU memory shows up (§3).
Drop `ib` and the GPU registration becomes invisible, not merely unattributed.

**Rejected:**

- *Filter the orphan rows.* `entity_close_accounted`'s own header argues against
  this at length: an unmatched close is the cheapest detector for a creation
  door nobody hooked, and suppressing the row discards the signal.
- *Teach the engine to record verbs QP opens too, so a mlx5dv-only run is
  self-consistent.* **This is the more principled fix and it is not done.** It
  is real work in the provider/engine rather than one word in a job script. If
  someone wants a capture that is complete under a single provider, this is the
  thread to pull.

## 3. GPU-residency of NIC buffers is a declared property per case

`workload.umem_control_plane_only = 1` in a case's `meta.json` tells
`umem_binding_as_configured` to accept a GPU-resident `ib_mem_region` where it
would otherwise demand a GPU-resident umem.

**Constraint: cases 09, 10 and 12 must NOT set it.** There the claim is
specifically that the *work queues* are in GPU memory — that is what "IBGDA
engaged" means, for GPU-submitted and CPU-assisted alike. Accepting an MR
instead would pass a capture whose queues are host-resident on the strength of
its data heap. du04 shows the discrimination is real and not theoretical:

| case | transport | umems | residency |
|---|---|---|---|
| 09 `ibgda_putget` | IBGDA, GPU-submitted | 10 | all GPU |
| 10 `ibgda_peermem` | IBGDA, by_addr | 10 | all GPU |
| 11 `ibdevx_peermem` | ibdevx, CPU-driven | 20 | all host, + a 512 MiB GPU-resident MR |

**Rejected, both of which were committed before being measured:**

- *Mark the assertion `expect_fail_asserts`* (`2b7e562`). Wrong mechanism —
  `sql_check.sh` documents it as "known to fail against a live bug", and this is
  a transport's registration strategy. Worse, it would have left the assertion
  permanently `xfail` on MN5, so the day a by_dmabuf capture got read with
  by_addr expectations — the exact mistake the assertion was written after — the
  board would still have said `xfail`.
- *Skip the case when `PeerMappingOverride=1` is absent* (`09fb091`). Built on
  the inference that MN5's 436 umems at `device=65535` meant peermem alone could
  not register GPU memory. **du04 has the flag and fails identically.** The
  driver flag was never the discriminator; the assertion was reading the wrong
  table. Reverted in `b1e244a`.
- *Relax the clause globally instead of per case.* See the constraint above.

## 4. `run_on_a100.sh` is a gate, not a convenience

It now builds `-DPCOMM_PROVIDER_IBVERBS=ON` alongside mlx5dv, and it exits
non-zero on any `ok:false` or missing result, scoped to the cases that
invocation ran.

**Constraint: a case that asks for a provider this runner does not build is a
hard stop**, not a degraded capture — the launcher refuses with "ib provider
library not found". Cases 11, 12 and 25 ask for `ib,mlx5dv`; the runner's
default is `--cases "09"`, so nothing exercises the others by accident.

This matters more than it looks: du04 is the only machine where 09, 10 and 11
ever really execute, and until `5c945d4` the runner could not go red at all. All
three had been reporting SKIP or FAIL from harness gates for their entire
existence and had never once run their assertions.

## Dead ends, so nobody re-derives them

- Register pressure in the injected function (swept 16..255 — no effect).
- `NVSHMEM_SYMMETRIC_SIZE` as the reason a run will not fit on a shared GPU
  (64 MiB fails identically to the 1 GiB default; the CUDA context alone is
  ~940 MiB of the ~2 GiB that was free).
- `PeerMappingOverride` as the reason NIC buffers are not GPU-resident under
  ibdevx (§3).
- Reading a crashed NVSHMEM init as evidence about umem residency. A run that
  dies inside `team_internal.cpp` has only bootstrap control-plane umems and no
  queue pairs; it is not comparable to a completed run and says nothing.

## Still open

- ~~**The SigmaTrace ibdevx comparison has never run.**~~ **Ran 2026-09-14** —
  `tests/hpc/cases/11_ibdevx_peermem/parity/README.md`. Agreement on every class
  both tools record except `ibv_dereg_mr` (a SigmaTrace gap); neither tool sees
  the ibdevx data plane. It is the reason case 11 exists (`meta.json:
  comparison_note`). The 2026-08-10 parity run used
  `NVSHMEM_REMOTE_TRANSPORT=ibgda` — that is case 12's doorbell comparison, a
  different one. SigmaTrace is installed on du04 at `~/SigmaTrace`.
- **`run_on_a100.sh` writes no per-case `.log`.** It runs `job.sbatch` directly
  through `| tail -40`, so assertion detail exists only in the runner's stdout
  and the workload's own output is truncated away. MN5 gets its `.log` from
  `all_in_one.sbatch`, which this runner does not use.
- **simurgh-base is back** (Fedora 44, reachable over Tailscale, `~/pockt`
  synced) but its GPU driver is mismatched — `NVML: Driver/library version
  mismatch`, library 615.71 — so it has no usable GPU and no InfiniBand. It does
  have gcc/cmake/nvcc, which makes it the one box that can compile pcomm
  locally: `tools/build_all.sh` and the pre-commit hook should work there, and
  the `--no-verify` every commit in this range needed does not apply to it.
  Untested — nothing has been built there yet.
