# PLAN — a cudaMalloc is an object whichever provider ran

- **Created:** 2026-11-18 19:30 +03
- **Status:** IN PROGRESS.
- **Follows:** `PLAN-20260810-0505-object-tracking-is-not-a-provider-concern-done.md`,
  which moved object identity into the engine and put plain device allocations
  explicitly out of scope "until a non-gpumem capture needs plain device
  allocations named". This is that capture.
- **Related:** `PLAN-20260805-0053-entity-lifetimes` (resolving an address
  against the `[open, close)` window — deferred there, and this plan makes it
  matter more; see "Not in this plan").

## The measurement

LAMMPS r2, MN5 job 49200469, 16 ranks on 4 nodes, Kokkos/CUDA, GPU-aware Open
MPI over UCX, captured with `-p ucx,ucx_deep`:

| table | rows |
|---|---|
| `ucx_tag_send` | 13,589 — 10,080 of them from **device** buffers (GPU 0..3) |
| `uct_send` | 38,524 — the halo arrives as `get_zcopy` reads of the sender's buffer |
| `mem_object` | **0** |
| `object_access` | **0** |

Every halo message names the GPU buffer it left from, and the capture cannot say
which buffer that is, because nothing outside NVBit records `cudaMalloc`. The
Objects panel is blank on every MPI science-app capture in the corpus (LAMMPS,
QE, GROMACS), and the only way to fill it today is gpumem — ~435 s of NVBit
module lifting to learn a buffer's extent that one driver call returns.

## Why a symbol interposer on cudaMalloc is not the answer

nvcc links cudart **statically** by default, so `cudaMalloc` never crosses a
dynamic symbol. cudart (static or shared) reaches the driver through
`cuGetProcAddress[_v2]` / `cuGetExportTable`, and both return what libcuda's own
pointer cell for that entry point holds (measured on du04,
`tests/hpc/probes/libcuda_cells`). bd895fc already overwrites those cells for
the dmabuf conversion — but the patcher lives in the mlx5dv provider, as a
header, so every .so that included it would carry its own copy and its own idea
of what "real" is. That is the shape that produced the self-recursion of MN5 job
48812008.

## Design

1. **One libcuda cell patcher, in the engine.** Move
   `providers/mlx5dv/cuda_export_table.hpp` into `libcomshu/src/libcuda_cells.cpp`
   behind the C ABI (`comshu_engine_libcuda_overwrite / _real / _patch`).
   `real()` resolves through the libcuda handle, never `RTLD_DEFAULT` — once the
   engine defines driver symbols, `RTLD_DEFAULT` names the engine. A second
   registrant for one entry point is refused, loudly: two owners of one cell is
   the design error the dlsym chain comment warns about. mlx5dv keeps its
   `export_table::` spelling as a shim, so uar_hijack and the dmabuf door are
   unchanged.

2. **The engine observes the driver allocation family** through three doors,
   tallied per door at shutdown like dmabuf:
   - **cell** — libcuda's pointer cells: cudart static or shared, NCCL,
     NVSHMEM, anything that asks `cuGetProcAddress`.
   - **dlsym** — `dlsym(libcuda, "cuMemAlloc_v2")`.
   - **link** — the engine defines the symbols, for code linked `-lcuda`
     (UCX's cuda transports, driver-API applications).

   Entry points: `cuMemAlloc_v2`, `cuMemAllocPitch_v2`, `cuMemAllocManaged`,
   `cuMemAllocAsync[_ptsz]`, `cuMemAllocFromPoolAsync[_ptsz]`, `cuMemAllocHost_v2`,
   `cuMemHostAlloc`, `cuMemMap` and the frees `cuMemFree_v2`,
   `cuMemFreeAsync[_ptsz]`, `cuMemFreeHost`, `cuMemUnmap`. `cuMemHostRegister`
   is not an allocation and stays mlx5dv's.

   Observers only: every wrapper returns exactly what the driver returned. A
   thread-local depth guard makes a nested call (a door wrapping a door, or the
   driver calling itself through a cell) pass straight through, so one call is
   one record. A close is emitted BEFORE the real free, as `nvshmem_free` does.
   `COMSHU_ALLOC_DOORS=0` disarms the cell and dlsym doors and makes the link
   door a pure pass-through.

3. **The registry knows liveness.** Entries are never erased (an access still
   draining after a free must still resolve), but they carry `live`, so a close
   of an already-closed base emits nothing, `cuMemUnmap` closes exactly the
   live mappings inside its range, and a reallocation at a dead (base, size)
   revives the entry instead of growing the table forever. `find()` prefers the
   innermost LIVE object and falls back to the innermost ever seen.

4. **A memory object says what made it.** `MemObject.origin : ObjectOrigin`, in
   the tail padding (stride unchanged), set by every emitter. An application
   allocation (`CuMemAlloc`, `CuMemAllocManaged`, `CuMemAllocAsync`,
   `CuMemHost`, `CuMemMap`, `NvshmemMalloc`) and a transport registration
   (`IbRegMr`, `DevxUmem`, `NcclWindow`) are different things that share a
   record type — the PLAN-0505 lesson — and until now only a buffer's size gave
   away which it was. Schema hash bump.

5. **UCX addresses resolve to objects.** `object_access` gains:
   - `ucx_rma` — a `ucp_put`/`ucp_get`'s `remote_addr`, in the peer's space
     (peer = `_ucx_peer_of_ep`), innermost containing object. Exact, like verbs.
   - `ucx_send` — a tag/active-message send's `buffer`, resolved in the
     SENDER's own objects: the object the payload left from, with the receiving
     rank as the actor. Under rendezvous that is literally who read it (the
     receiver's `get_zcopy`); under eager it is who the bytes were for.

   And a `unit` column, because a UCP count is datatype units and the Objects
   panel sums `size` as bytes — one column must not silently carry two
   quantities (the same reason `comm_edges` has `unit`).

6. **UI.** Objects shows the origin and labels non-byte sizes; Devices' ucx tab
   names the object a send left from.

## Verification

- simurgh: build, unit tests, the registry's liveness rules, schema conformance.
- MN5 probe job: one tiny program per route — static cudart, shared cudart,
  driver API linked `-lcuda`, driver API via `dlsym`, VMM, managed, async,
  pinned host — each must record its objects, through the door it took, with
  no double record. Plus an mlx5dv control to show the shared patcher kept the
  dmabuf door working.
- MN5 LAMMPS r2 rerun with `-p ucx,ucx_deep`: `mem_object` populated from
  `cuMemAlloc` with no gpumem, and halo sends resolving to Kokkos buffers.

## Not in this plan

- **Lifetime-window resolution in the reader.** Every `object_access` branch
  takes the smallest containing range ever recorded. With device allocations
  tracked, a freed-and-reused address can resolve to a stale object. The fix
  belongs to all branches at once (PLAN-20260805-0053), not to UCX alone.
- **The receive side of UCX.** `ucx_tag_recv.buffer` resolves locally, but which
  rank a wildcard receive matched needs tag matching this view does not do.
- **`uct_send` get addresses.** A UCT endpoint names no peer rank.
