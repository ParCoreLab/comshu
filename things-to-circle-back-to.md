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

**Not placed:** all 81,920 puts are `unplaced_offset_sends`. ROOT CAUSE,
measured with gdb on rank 0 (2026-11-11): NCCL 2.28.9 calls libcuda's exported
`cuMemGetHandleForAddressRange` directly, through a pointer it gets at
communicator init via cudaGetDriverEntryPoint, and from two call sites (core
and its embedded DOCA). The process makes 15 `cuGetExportTable` calls: 7 from
the app's libcudart.so.13, 7 from libnccl's static cudart, and **1 through our
export-table door, LAST**, for a table neither cudart asked for. So the door's
patch lands after every pointer that matters has already been handed out. On
MN5 (CUDA 12.9) the order happened to favour us (export_table=33 there).

What the capture does have, since 2026-11-11: `nccl_window.base`, a MemObject
per window, and `ib_mem_region.dmabuf_offset` plus a registration time. Those
showed that window-based placement CANNOT be exact here. GIN's registrations
happen after both window calls, every slice has dma-buf offset 0, and "fd 99"
is reused by successive exports. So only the conversion itself knows a slice's
address.

**Next, options (decision pending):**
(a) patch earlier: trigger the export-table patch before NCCL initialises (from
the NCCL provider's `ncclCommInitRank` pre-hook, or at libcuda load). The
patcher's walk relies on overshooting a table's sentinel (159,612 entries on
du04), which is fragile, so this needs a principled table walk first.
(b) patch the CALLER's cell: after `ncclCommInitRank`, overwrite pointer cells
in libnccl's writable segments that hold the real
`cuMemGetHandleForAddressRange` with our wrapper. Targeted, but conversions
made during init are missed.
(c) inline-hook libcuda's exported function (a trampoline). Covers every route
on every host; the most invasive option.

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
