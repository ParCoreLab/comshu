# pcomm — things to circle back to

Known limitations and deferred work: one paragraph each, with the evidence and
the next step. Only OPEN items live here. When one is fixed, its entry is
deleted in the commit that fixes it, and the commit message carries the story.
Everything closed before 2026-11-09 was cut from this file then. Code comments
that cite it for a FIXED or closed entry refer to that full version:
`git show c135365:things-to-circle-back-to.md` (1,561 lines).
Defects in other people's software go to `BUGS-FOUND.md`; designs go to `plans/`.

## The first full arena stalls the thread that fills it for ~2 ms

`pcomm_engine_arena_flush` runs inline on the recording thread
(`libpcomm/src/engine.cpp`), and the first flush also does the one-time
`GpuDevice` (a CUDA driver query) and `Invocation` emission. Measured on du04 on
the send-queue watcher: that batch took 2.1–2.4 ms, and every other batch took
microseconds. On an application thread that's harmless. On a doorbell repeater
it isn't: the repeaters record before they forward (`memory_repeater.hpp`), so
the first full arena holds one of the application's doorbells for ~2 ms. The
published hijack costs (+68% forward, +86% with decode) may include it. **Next:**
move the one-time emissions and the flush off any thread whose latency belongs
to someone else, then measure the repeater path.

## NCCL GIN's GDAKI plane is not captured on du04

It is on MN5 (case 26, through the host doorbell). On du04 neither doorbell arm
sees it: the GPU arm installs and remembers 32 UARs but sees
`register_calls=0 getdevptr_calls=0`, and the host arm correctly refuses because
du04 can map a UAR into the GPU. DOCA logs "Enabling CPU proxy mode" only when
the handler is pinned to CPU. Case 26 skips on du04 and says why. **Next:** an
explicit opt-in that arms the host path when the handler is pinned to CPU by
configuration (safe, because DOCA then never probes for GPU mapping). Then
re-check whether the GPU arm is now reached through the export-table door
(`providers/mlx5dv/dmabuf_door.hpp` showed that is how cudart_static reaches the
driver on MN5).

## Instrumenting one local-memory load wedges NVSHMEM's IBRC proxy

Bisected on MN5 (jobs 43918460..43920181, `PCOMM_INSTR_LO/HI`) to a single
instruction: instrumenting `[0,1006)` of `nvshmemi_transfer_rma_p<int>`
completes, `[0,1007)` hangs. Index 1006 is `LDL.64` at `+0x19680`, a spilled-register
reload. The surrounding global atomics and system-scope stores are innocent. It
needs NVSHMEM's IBRC proxy (reproduces on one node with `NVSHMEM_DISABLE_P2P=1`)
and needs the injected `push()` to run (`-DPCOMM_INJECT_NO_PUSH` completes). The
channel never fills, no host thread is blocked, and the kernel issues every
access it should and then never exits. The case 07 hang is avoided on our side;
the **mechanism is unknown**, which is why this is not a `BUGS-FOUND.md` entry:
it could be NVBit's, NVSHMEM's or ours. **Next:** a minimal NVBit tool that
injects a call before that one load, outside pcomm. If it hangs, it goes to
NVBit/NVSHMEM with the reproducer.

## Goldens still encode run-specific identifiers

26 goldens remain, in cases 01–08 and 20. Several have failed on identifiers the
allocation happened to hand out (both ranks given `qp_num 449`, a peer order
moving), not on anything the tool did. Each affected case already has an
assertion beside the golden that states the shape instead
(`PLAN-20260803-1520-correctness-checks-not-goldens-done`). **Next:** retire
each golden that its assertions fully cover, case by case, and keep a golden
only where it pins a number no assertion can derive.

## `run_on_a100.sh` syncs from a syncthing-fed checkout

A du04 board can run a mixed tree, because a provider and the assertion beside
it can arrive separately. `run_on_mn5.sh` can't do this (it pushes HEAD over
git and refuses a dirty tree). The A100 runner prints the source commit, flags
a dirty tree and honours `PCOMM_EXPECT_COMMIT`, but the hop is still
asynchronous. **Next:** sync du04 over git the way MN5 is synced.
