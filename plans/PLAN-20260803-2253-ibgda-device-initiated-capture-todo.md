# PLAN — capture device-initiated IB (IBGDA), which pcomm currently records as silence

- **Created:** 2026-08-03 22:53 +03
- **Status:** TODO
- **Prior art:** `../SigmaTrace` — `Mlx5dvTrace/`, `Profiler/Handlers/IBV/Mlx5dv.cpp`,
  `Profiler/Handlers/CUDAHijack.cpp`, `Profiler/Modules/MemoryHijack/`,
  `Profiler/Modules/CudaExportTableOverwrite/`, `Profiler/Modules/GPUMemoryHijack/`.
- **Related:** case 08's empty `ib_queue_pair` (`mlx5dv_create_qp` unhooked) is the
  same missing provider, one layer up. `PLAN-20260803-1448-nvshmem-heap-attribution`
  is the cautionary tale for version-dependent introspection of NVSHMEM internals.

## What is broken

pcomm's ibverbs provider hooks the CPU's data plane: `ibv_post_send` and
`ibv_poll_cq` (`providers/ibverbs/ib_hooks.hpp:14-21`). In IBGDA there is no CPU
data plane. NVSHMEM's GPU-initiated path has a *device* thread build the WQE
directly in the send queue and ring the doorbell itself:

```
nvshmemi_ibgda_device_qp_t *qp, uint16_t prod_idx) {
    uint64_t *bf_ptr = (uint64_t *)qp->tx_wq.bf;
    ibgda_ctrl_seg_t ctrl_seg = { .opmod_idx_opcode = HTOBE32(prod_idx << 8),
                                  .qpn_ds          = HTOBE32(qp->qpn << 8) };
    ibgda_store_release(bf_ptr, *((uint64_t *)&ctrl_seg));
}
```

*(NVSHMEM `ibgda_device.cuh`, quoted in SigmaTrace `CUDAHijack.cpp:349-362`.)*

The SQ lives in GPU HBM and the UAR doorbell page is mapped into the GPU's
address space, so the entire operation is one GPU store to a BAR. The CPU is
never involved and `LD_PRELOAD` has nothing to interpose on.

**The control plane is missing too**, which is the part that surprised me. IBGDA
does not create its QPs with `ibv_create_qp` and does not connect them with
`ibv_modify_qp` — it drives DEVX directly:

| what | ordinary verbs | IBGDA | pcomm today |
|---|---|---|---|
| register memory | `ibv_reg_mr` | `mlx5dv_devx_umem_reg` | miss |
| create QP | `ibv_create_qp` | `mlx5dv_devx_obj_create` + `MLX5_CMD_OP_CREATE_QP` | miss |
| connect | `ibv_modify_qp` | `mlx5dv_devx_obj_modify` + `MLX5_CMD_OP_INIT2RTR_QP` | miss |
| doorbell page | — | `mlx5dv_devx_alloc_uar` + `cuMemHostRegister`/`cuMemHostGetDevicePointer` | miss |
| send | `ibv_post_send` | GPU store to UAR | miss |

So an NVSHMEM-over-IB run through pcomm today yields **no QPs, no topology, no
operations** — and yields them *silently*, as empty tables. That is precisely the
failure family the last three days of work has been about: case 08's empty
`ib_queue_pair` passing against an empty golden, case 05's phantom loads. An
absent provider and a broken one look identical downstream. Whatever ships here
must make "IBGDA was active and I captured nothing" an error, not a blank table.

Note that this is *not* the same gap as the intra-node NVLink path. Case 07 and
the gpumem provider already see GPU-initiated NVLink stores through NVBit. The
hole is specifically GPU-initiated traffic **that leaves the node through the
NIC**, which is exactly the traffic pcomm exists to explain.

## How SigmaTrace does it

Three layers, only the first of which is passive.

### Layer 1 — DEVX control plane, by preload hook (passive)

`Mlx5dvTrace/Mlx5dvHooks.hpp` preloads eight symbols; `Mlx5dv.cpp` decodes the
DEVX command buffers with the PRM accessor macros:

- `mlx5dv_devx_umem_reg` / `_dereg` → every registered buffer. A
  `cudaPointerGetAttributes` on the address tells whether it is GPU memory, and
  the umem id → (device, memtype, CUcontext) map is what later identifies a QP as
  device-driven (`Mlx5dv.cpp:296-331`).
- `mlx5dv_devx_alloc_uar` / `_free_uar` → the doorbell page, `reg_addr` and
  `page_id`.
- `mlx5dv_devx_obj_create` with `MLX5_CMD_OP_CREATE_QP` → `qpn` from the output
  buffer, `wq_umem_id` + `wq_umem_offset` (giving `sq_base` in GPU memory),
  `log_sq_size`, and `st`, the transport service type (`0x5` = DCI).
  Crucially it *returns early* if the WQ umem was not a GPU buffer
  (`Mlx5dv.cpp:82-87`) — a CPU-side DEVX QP cannot produce GPU doorbells.
- `mlx5dv_devx_obj_modify` with `MLX5_CMD_OP_INIT2RTR_QP` → `remote_qpn`, and
  from `primary_address_path`: `rlid`, `vhca_port_num`, and the 16-byte remote
  GID. This is the IBGDA equivalent of the `IbModifyQp` RTR row we just fixed
  with `src_lid`.

This layer needs mlx5 internal headers (`mlx5_ifc.h`, `mlx5_prm.h`) for the
`DEVX_GET`/`DEVX_ADDR_OF` field accessors. SigmaTrace vendors them from
`nvshmem/src/modules/transport/common/` (`Profiler/mlx5/README.md`). They are
dual GPL-2.0/OpenIB-BSD, so vendoring under the BSD option is fine.

### Layer 2 — UAR redirection, by CUDA driver interception (invasive)

This is the load-bearing trick, and it is worth stating plainly because it is
where the design cost sits.

1. NVSHMEM allocates the UAR (`mlx5dv_devx_alloc_uar`) — recorded, address kept.
2. NVSHMEM calls `cuMemHostRegister(uar->reg_addr, …, CU_MEMHOSTREGISTER_DEVICEMAP)`
   to map the BAR page for the GPU. SigmaTrace's replacement **skips the real
   call** and just notes the address (`CUDAHijack.cpp:528-543`).
3. NVSHMEM calls `cuMemHostGetDevicePointer(&dptr, uar->reg_addr, …)`. The
   replacement allocates a **fake 4 KiB pinned page**, registers a repeater for
   it, and returns the device pointer *of the fake page*
   (`CUDAHijack.cpp:467-519`).

The GPU's doorbell store now lands in host memory that pcomm owns. A per-UAR
host thread (`MemoryRepeater.hpp:41-80`) parks on the address with
`UMONITOR`/`UMWAIT` (or AMD `MONITORX`/`MWAITX`, else `PAUSE`), and on each
change: **calls the callback first, then forwards the value to the real NIC
UAR** — that ordering is deliberate, so the WQEs can be copied out of the SQ
before the NIC consumes them and the GPU recycles the slots.

Intercepting the driver API at all requires a subsystem pcomm does not have:
`Profiler/Modules/CudaExportTableOverwrite/` hooks `cuGetExportTable` and
`cuGetProcAddress_v2`, walks the returned function table to a `0xffff…ffff`
sentinel, `mprotect`s the pages and patches entries in place
(`ExportTablePreload.cpp:90-147`). CUDA hands out its entry points through both
doors, and the file's own comment says patching one is not enough:
*"I don't know why, but we have to replace the function here too, even if it was
replaced in cuGetExportTable. Otherwise we get CUDA_ERROR_INVALID_CONTEXT
sometimes."*

### Layer 3 — WQE decoding

`on_doorbell_ring` (`CUDAHijack.cpp:349-466`) byte-swaps the 8-byte doorbell into
`qpn` and `prod_idx`, diffs against the last producer index, `cuMemcpyAsync`s the
delta WQEBBs out of the GPU SQ into a pinned host buffer on a per-QP stream, and
parses ctrl → (raddr) → data segments for opcode, `raddr`/`rkey`,
`laddr`/`lkey`, and `byte_count`. Batching threshold 128 WQEBBs, flushed on
`cuStreamSynchronize`/`cuCtxSynchronize`/UAR free.

**What SigmaTrace has that does not actually run.** The GPU-resident variant —
a persistent kernel polling the fake doorbell in HBM and parsing WQEs on-device
(`GPUMemoryRepeater.cu:147-247`) — forwards the doorbell and then `continue`s
straight past the parse block; `parse_and_record` is called from nowhere;
`parse_and_record_oneshot` has its one call commented out (line 138); there is a
bare `printf("UPDATING\n")` in the poll loop. **Do not read that file as a
reference implementation.** The host repeater is the path that works.

## What pcomm has, and what it lacks

Have:
- Preload hook machinery that takes an arbitrary symbol
  (`DEFINE_PRELOAD_HOOK`), and `smart_dlsym`. Adding the eight `mlx5dv_devx_*`
  hooks is mechanically identical to the ibverbs ones.
- A reserved id range: `id_range::mlx5dv_lo = 200, mlx5dv_hi = 299`
  (`libpcomm/include/pcomm/core/ids.hpp:27`). Already allocated, never used.
- A provider ABI (`PCOMM_EXPORT_PROVIDER`), the arena/recorder path, and the
  reader's `Table<R>` descriptor pattern — a new provider is additive.
- An NVBit backend that already instruments every global memory instruction and
  already resolves device ownership of an address.

Lack:
- **Any CUDA driver interception whatsoever.** No `cuGetProcAddress` hook, no
  `cuGetExportTable` hook, no export-table patcher. `grep` finds zero hits.
- Any mlx5/DEVX knowledge.
- Any notion of a provider that *changes* what the application does. Every hook
  today is call-through-and-record.

## Design options for layer 2

### A. Port SigmaTrace's host repeater

Known to work on this hardware. Also: puts a host thread on the critical path of
every device-initiated send. The doorbell goes from one GPU store to
`store → host wakeup → host store`, which is a latency change of a different
order than pcomm's usual overhead, on the one path whose whole reason for
existing is that it bypasses the host. It cannot honestly be default-on, and any
timing conclusion drawn from a run under it is about the instrumented system.

There is also a correctness question SigmaTrace itself flags but does not
resolve — `MemoryRepeater.hpp:59`: *"This is possibly still not good!
GPU-PCIe-NIC ordering is different from CPU"*. Redirecting a release store from a
BAR page to host memory and replaying it from a different agent changes who
guarantees the SQ writes are visible to the NIC before the doorbell is. Getting
this wrong does not corrupt the trace, it corrupts the *application*.

### B. NVBit-native passive capture

**Cost first, because it decides when this is even arguable.** The hijack is
cheaper. It costs a host thread per UAR and one wakeup per doorbell. NVBit
instruments *every* global memory instruction in *every* kernel — that is the
gpumem provider's cost, which case 05 charges to preparation rather than suite
time precisely because it is not small. As a standalone mechanism for capturing
IBGDA, B loses and it is not close.

B is only arguable on **marginal** cost: in a capture that is already running
NVBit — gpumem and ibverbs together, which is exactly case 06 — the doorbell
filter is nearly free, and it is passive. So the honest framing is not "B instead
of A" but "if NVBit is already paying for itself, B gets device-initiated IB for
almost nothing and without touching the application." Whether that case is common
enough to build for is a judgement call, not a measurement.


pcomm already runs NVBit over these kernels. The doorbell is a GPU store to a
known address, and NVBit sees every store. We know the UAR device pointer (layer
1 tells us the UAR; the `cuMemHostGetDevicePointer` return value tells us where
it landed) and we know each SQ's `[sq_base, sq_base + (1<<log_sq_size)*64)`.

Today's instrumentation passes address, size and opcode but **not the stored
value** (`gpu-backends/nvbit/inject_funcs.cu:12-15`, `dev_common.h:10-18`) — so
as-is it can say *a doorbell was rung* and when, but not for which QP or up to
which producer index. NVBit can pass register values
(`nvbit_add_call_arg_reg_val`), so the extension is: for stores, also pass the
data register(s), or pass them only for a filtered address range.

If value capture works, this gets the same `(qpn, prod_idx)` the hijack gets,
**with no redirection, no host thread, no driver patching, and no change to
application behaviour** — and it composes with the SQ writes, which NVBit also
sees, so the WQE contents could be reconstructed from the stores that built them
rather than raced for afterwards.

Unknowns to settle before committing: whether the ibgda WQE/doorbell stores are
in an instrumented (non-inlined-into-a-skipped-module) code path at all; whether
register-value capture is affordable at this instruction count; and whether the
128-bit WQE stores need multi-register capture. **Step 1 of this plan is a probe
that answers those, not a commitment.**

### C. Control plane only

Ship layer 1 alone: QPs, topology, memory registrations, doorbell page — but no
operations. Not a full answer, but it is honest, passive, cheap, and it makes the
IBGDA connection graph visible where today there is nothing. It is also a strict
prerequisite for both A and B, since both need `qpn → sq_base` and the UAR
address. **Everything in Stage 1 below is needed regardless of which of A or B
wins**, which is why it goes first.

## Plan

### Stage 1 — the mlx5dv/DEVX provider (passive, no behaviour change)

New `providers/mlx5dv/`, modelled on `providers/ibverbs/`, ids 200-299.

1. `mlx5dv_hooks.hpp` — `DEFINE_PRELOAD_HOOK` for `mlx5dv_devx_umem_reg`,
   `mlx5dv_devx_umem_dereg`, `mlx5dv_devx_alloc_uar`, `mlx5dv_devx_free_uar`,
   `mlx5dv_devx_obj_create`, `mlx5dv_devx_obj_modify`, `mlx5dv_open_device`,
   and `mlx5dv_create_qp` (which also closes case 08's gap).
2. Vendor `mlx5_ifc.h` / `mlx5_prm.h` under `third_party/mlx5/` with provenance
   and the BSD election recorded, and a `DisableFormat: true` `.clang-format`
   beside them like the other vendored trees.
3. Records (draft):
   - `Mlx5Umem` (Entity, 200) — `umem_id`, `range`, `device`, `mem_space`.
   - `Mlx5Uar` (Entity, 202) — `uar` handle, `reg_addr`, `page_id`.
   - `Mlx5DevxQp` (Entity, 204) — `qpn`, `sq_base`, `log_sq_size`,
     `service_type`, `wq_umem_id`, `device`.
   - `Mlx5DevxQpConnect` (Event, 206) — `qpn`, `dest_qpn`, `dest_lid`,
     `src_lid`, `port_num`, `dest_gid_prefix`, `dest_gid_iface`.
     Carries `src_lid` from the outset — the collision this fixes in
     `IbModifyQp` (`758e975`) is the same collision here, and there is no reason
     to relearn it.
   - `Mlx5Doorbell` (Event, 210) — reserved for Stage 3; not emitted yet.
4. **Refusal, not silence.** If a DEVX opcode decodes to something the vendored
   layout does not recognize, or `wq_umem_id` names a umem we never saw, emit a
   diagnostic record and count it. `find_nvshmem_heap_owner` is the standing
   lesson: introspection into another project's internals must fail loudly at the
   version boundary. A run where NVSHMEM changed its DEVX usage must produce a
   red case, never a plausible-looking short table.

**Exit criterion:** on a 2-node IBGDA NVSHMEM run, per-rank DEVX QP counts are
symmetric, the connection graph is a complete ring or full mesh as the workload
dictates, and every device-driven QP's `sq_base` resolves to GPU memory on the
expected device.

### Stage 2 — probe whether NVBit can see the doorbell (spike, timeboxed)

Not a deliverable; a decision. With Stage 1 giving the UAR device pointer and SQ
ranges, run case 04-style NVSHMEM put/get under the existing gpumem provider and
answer three questions:

1. Do any instrumented accesses land in the UAR page or the SQ ranges? (Pure
   address filtering — no new instrumentation needed to answer this.) If none do,
   option B is dead and the answer is A; say so and stop.
2. Can `nvbit_add_call_arg_reg_val` deliver the 8-byte doorbell value, and at
   what cost at this instruction count?
3. Do the WQE-building stores appear, and with what widths?

Record the answer in this file before writing Stage 3.

### Stage 3 — the data plane, by whichever route Stage 2 chose

**If B:** extend `pcomm_gpu_access_t` with an optional value payload, gate it
behind a device-side address-range filter so only UAR/SQ traffic pays for it,
decode `(qpn, prod_idx)` host-side, and emit `Mlx5Doorbell`. Fully passive.

**If A:** the larger build. In order — (i) a CUDA export-table interception
module, which is a genuinely new subsystem and should land with its own tests
before anything depends on it; (ii) the UAR fake-page substitution; (iii) the
host repeater thread; (iv) WQE copy-out and decode. **Off by default**, behind an
explicit `pcomm --enable-doorbell-hijack`-style flag, with the perturbation
stated in the launcher's own output — a user must not be able to draw a latency
conclusion from a hijacked run without having been told.

### Stage 4 — tests, derived rather than golden

Consistent with `PLAN-20260803-1520-correctness-checks-not-goldens`. New case
`09_ibgda_putget`, 2 nodes, `NVSHMEM_IB_ENABLE_IBGDA=1`,
`NVSHMEM_IBGDA_NIC_HANDLER=gpu`, small fixed-size puts in a known pattern.

- **Engagement guard, first:** the case must prove IBGDA was actually active —
  at least one DEVX QP whose SQ is in GPU memory. If NVSHMEM silently fell back
  to the CPU proxy transport, the case fails loudly. Without this every other
  assertion below is vacuously true on a fallback run, which is exactly how case
  08 hid.
- Per-rank symmetry of DEVX QPs, umems, and connections — min == max, none zero,
  `ranks_seen == params.ranks` (the `06` pattern).
- Connection graph: every `dest_qpn` resolves to a QP some rank actually created;
  no self-connections; degree matches the workload's neighbour count.
- Stage 3 only: doorbell count and summed `byte_count` equal the values derived
  from the application's own put count and element size — the deduced-expected
  check, not a recorded total.
- Stage 3 route A only: a run with and without the hijack must agree on
  everything except timing.

## Risks

- **Version fragility.** DEVX command layouts and NVSHMEM's use of them are not a
  stable interface. Mitigated by refusal-not-silence (Stage 1.4) and by keeping
  every layout assumption in one decoding file.
- **Behaviour change (route A only).** The hijack alters the application's
  doorbell path and possibly its ordering guarantees. Opt-in, loud, and never
  used to produce a timing claim.
- **Export-table patching (route A only).** `mprotect` + in-place patching of a
  table whose length is found by scanning for a sentinel is the least defensible
  code in SigmaTrace, and it would be new to pcomm. It deserves its own tests and
  its own kill switch.
- **Scope.** Stage 1 alone is a real deliverable and closes a genuine hole
  (including case 08). Stages 2-4 should not hold it up.
