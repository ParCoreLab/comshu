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

**Placed** since 2026-11-12. Before that all 81,920 puts were
`unplaced_offset_sends`: NCCL 2.28.9 got the real `cuMemGetHandleForAddressRange`
through cudaGetDriverEntryPoint before our export-table door ever patched
anything (it saw the last of 15 `cuGetExportTable` calls). The door now
overwrites libcuda's own pointer cell, before any pointer-getter is handed out
(`providers/mlx5dv/cuda_export_table.hpp`, evidence in
`tests/hpc/probes/libcuda_cells`). That reaches every asker, however cudart or
NCCL is linked, and needs no table walk. du04: `libcuda_cell=28`, registrations
112/112 resolved (was 88/24), and all 40,960 device sends per rank land exactly
in the second NCCL window (128 B writes, offsets 0..131072) and GIN's 512 KiB
signal buffer (8 B atomics).

**GPU arm (handler on the GPU):** NCCL `dlsym`s `cuMemHostRegister_v2` and
`cuMemHostUnregister` from libcuda's handle (`COMSHU_TRACE_DLSYM=1`: both
UNHOOKED). It also never calls `cuMemHostGetDevicePointer`, because under UVA
the registered UAR address IS the device address. So hooking the `dlsym` door
is not enough: the hijack swaps the page at getdevptr time, a call that never
comes. **Next:** a swap at REGISTER time. Move the UAR mapping aside
(mremap), put a watched anonymous page at its address, and register that one
instead. That needs its own plan; it moves a device mapping under a running
transport.

## `run_on_a100.sh` syncs from a syncthing-fed checkout

A du04 board can run a mixed tree, because a provider and the assertion beside
it can arrive separately. `run_on_mn5.sh` can't do this (it pushes HEAD over
git and refuses a dirty tree). The A100 runner prints the source commit, flags
a dirty tree and honours `COMSHU_EXPECT_COMMIT`, but the hop is still
asynchronous. **Next:** sync du04 over git the way MN5 is synced.
