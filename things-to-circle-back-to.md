# comshu — things to circle back to

Known limitations and deferred work: one paragraph each, with the evidence and
the next step. Only OPEN items live here. When one is fixed, its entry is
deleted in the commit that fixes it, and the commit message carries the story.
Everything closed before 2026-11-09 was cut from this file then. Code comments
that cite it for a FIXED or closed entry refer to that full version:
`git show c135365:things-to-circle-back-to.md` (1,561 lines).
Defects in other people's software go to `BUGS-FOUND.md`; designs go to `plans/`.

## A flush still writes on the thread that filled the arena

The one-time emissions (GpuDevice and Invocation, 2.1–2.4 ms on du04's
send-queue watcher) moved to an engine thread on 2026-11-11, so the FIRST flush
is no longer special. On university-base, `first_flush_latency` measures the
first flush at 243–290 µs (it was 418–462 µs) against 177–246 µs for later ones.
What remains is the flush's own write, which every batch pays on whatever thread
filled it. On a doorbell repeater that is still an application doorbell held
for the length of a write. **Next:** measure the repeater's per-flush hold on
du04 and decide whether it needs an asynchronous writer (hand the full arena to
an engine thread and swap in an empty one).

## NCCL GIN on du04: captured with the CPU handler, not placed, and the GPU arm is blind

**Captured** when the GDAKI handler is pinned to the CPU (2026-11-11): with
`NCCL_GIN_GDAKI_NIC_HANDLER=1` and the opt-in `COMSHU_HOST_DOORBELL_CPU_HANDLER=1`,
`--enable-host-doorbell` arms all 24 UARs and records 40,960 sends per rank
(20,480 × 128 B writes = 2,621,440 B, as on MN5), every peer resolved,
`unresolved_sends` 0, and the application verifies every byte. The probe is
`/DATA/missa18/gin/du04_probe4.sh` (NCCL 2.28.9 from /usr; the SDK's 2.27.7
has no device API, so case 26 itself still skips on du04).

**Not placed:** all 81,920 puts are `unplaced_offset_sends`. The window is
registered zero-based with dma-buf fd 98/99, and no door ever sees the
conversion that made those fds. `COMSHU_TRACE_DMABUF=1` shows only DOCA's small
4/8 KiB conversions arriving, all through `dlsym` (168). NO `cuGetProcAddress`
door is ever asked for a `cuMem*` symbol, and the export-table door counts 0
where MN5 counted 33. NCCL core's conversions take a route we don't patch on
this host. **Next:** find that route (is the export-table patch installed
before libnccl's static cudart reads the table on du04?).

**GPU arm (handler on the GPU):** NCCL `dlsym`s `cuMemHostRegister_v2` and
`cuMemHostUnregister` from libcuda's handle (`COMSHU_TRACE_DLSYM=1`: both
UNHOOKED). It also never calls `cuMemHostGetDevicePointer`, because under UVA
the registered UAR address IS the device address. So hooking the `dlsym` door
is not enough: the hijack swaps the page at getdevptr time, a call that never
comes. **Next:** a swap at REGISTER time. Move the UAR mapping aside
(mremap), put a watched anonymous page at its address, and register that one
instead. That needs its own plan; it moves a device mapping under a running
transport.

## Instrumenting one local-memory load wedges NVSHMEM's IBRC proxy

Bisected on MN5 (jobs 43918460..43920181, `COMSHU_INSTR_LO/HI`) to a single
instruction: instrumenting `[0,1006)` of `nvshmemi_transfer_rma_p<int>`
completes, `[0,1007)` hangs. Index 1006 is `LDL.64` at `+0x19680`, a spilled-register
reload. The surrounding global atomics and system-scope stores are innocent. It
needs NVSHMEM's IBRC proxy (reproduces on one node with `NVSHMEM_DISABLE_P2P=1`)
and needs the injected `push()` to run (`-DCOMSHU_INJECT_NO_PUSH` completes). The
channel never fills, no host thread is blocked, and the kernel issues every
access it should and then never exits. The case 07 hang is avoided on our side;
the **mechanism is unknown**, which is why this is not a `BUGS-FOUND.md` entry:
it could be NVBit's, NVSHMEM's or ours. **Next:** a minimal NVBit tool that
injects a call before that one load, outside comshu. If it hangs, it goes to
NVBit/NVSHMEM with the reproducer.

## `run_on_a100.sh` syncs from a syncthing-fed checkout

A du04 board can run a mixed tree, because a provider and the assertion beside
it can arrive separately. `run_on_mn5.sh` can't do this (it pushes HEAD over
git and refuses a dirty tree). The A100 runner prints the source commit, flags
a dirty tree and honours `COMSHU_EXPECT_COMMIT`, but the hop is still
asynchronous. **Next:** sync du04 over git the way MN5 is synced.
