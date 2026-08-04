# PLAN — capture device-initiated IB (IBGDA), which pcomm currently records as silence

- **Created:** 2026-08-03 22:53 +03
- **Status:** STAGE 1 SHIPPED, stages 2-4 re-scoped by measurement. The
  mlx5dv/DEVX provider is in the tree, builds on MN5 and du04, and captures real
  GPU-initiated traffic: case 09 passes on du04 with **0 goldens and 9
  assertions**, all derived. What remains is the *data plane* — doorbells and
  WQE contents — where the A-vs-B choice is now informed rather than argued.
  See "What actually shipped" below.
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

## Stage 2 — what actually happened

Three jobs. The question the probe was built to answer was never reached,
because a prior question answered itself first.

**44156334** — aborted in 15 s, shim did not compile. `common.sh` exports
`NCCL_HOME` and `NVSHMEM_HOME` but never `CUDA_HOME`, so the build ran with
`-I/include`. Fixed to key off `PCOMM_SDK`; every later job test-compiled the
shim on the login node before submitting.

**44156479** — IBGDA failed to initialise:

```
ibgda.cpp:nvshmemt_init:3577: neither nv_peer_mem, or nvidia_peermem detected.
                              Skipping transport.
init failed for transport: IBGDA
```

Self-inflicted. `NVSHMEM_IB_DISABLE_DMABUF=1` was copied from SigmaTrace without
checking what it disables: IBGDA reaches GPU memory by DMABUF *or* by the peermem
kernel module, SigmaTrace sets that flag because their system lacks the first and
needs the second, and on MN5 it is the second that is missing. The flag left
IBGDA with no route at all.

The run also broke the probe's own guard. It logged **159 DEVX `CREATE_QP`
calls on a run where IBGDA never initialised** — HPC-X/UCX drives DEVX for
ordinary MPI — and the guard reported that as "IBGDA engaged". The guard existed
to stop exactly this inference and committed it anyway. Now keyed on
GPU-resident WQ umems (SigmaTrace's discriminator, `Mlx5dv.cpp:82-87`), with the
QP count printed and labelled *"NOT evidence on its own"*. Three further defects
the failure exposed — eight ranks shredding one shared log file, `classify()`
flattening "could not ask CUDA" and "CUDA says host memory" into the same `-1`,
and the analysis being skipped on a failed run when the failure *was* the
finding — are fixed in `8f452c1`.

**44156594** — DMABUF re-enabled. The failure moved deeper and became
conclusive:

```
ibgda.cpp 3837 no active IB device that supports GPU-initiated communication
               is found, exiting...
```

NVSHMEM enumerated the HCAs and none of them qualifies. That is a device/firmware
capability verdict, not a configuration mistake, and no environment variable
reaches it. Node `as06r3b28`: `nvidia_peermem` NOT LOADED,
`/sys/kernel/mm/memory_peers` absent.

Probe tooling confirmed sound on the way past: all 2496 umem registrations
classified `cuerr=1` (`CUDA_ERROR_INVALID_VALUE`) rather than `3`
(`NOT_INITIALIZED`) — CUDA was answering "host pointer", which is correct for
MPI's buffers, so the classifier works and would have recognised a GPU-resident
send queue had one existed. Per-rank logs came back clean for all 8 ranks.

### What this changes

The blocker is symmetric, and that is the load-bearing point. **Option A is no
more testable here than option B.** No IBGDA means no device-initiated doorbells
to hijack, so SigmaTrace's approach cannot be developed, validated, or regression-
tested on MN5 either. The cost argument that favours A is still correct and still
does not help: neither route can be exercised.

So the decision is no longer "A or B". It is:

1. **Find hardware that supports IBGDA.** SigmaTrace's doorbell hijack presumably
   ran somewhere — where? That cluster is the prerequisite for any of Stages 3-4,
   whichever option wins.
2. **Ask BSC** whether GPU-initiated communication is disabled by firmware/driver
   configuration on the ACC partition rather than absent from the hardware. The
   HCAs may be capable with `nvidia_peermem` loaded or a firmware setting changed.
3. **Ship Stage 1 regardless.** The mlx5dv/DEVX provider needs none of this. It is
   passive, it closes case 08's `mlx5dv_create_qp` gap, and this probe already
   proves there is a real DEVX control plane on MN5 to capture — 2880
   `devx_obj_create` calls, 1272 of them `CREATE_QP`, 816 UAR allocations, all
   currently invisible to pcomm. That traffic is MPI's rather than NVSHMEM's,
   which makes it *less* interesting than IBGDA but not uninteresting: it is
   real communication the tool cannot presently see.
4. **Do not build A or B blind.** Writing an untestable doorbell hijack — the one
   component that can corrupt the application rather than merely the trace — is
   the worst possible thing to develop without a machine to run it on.

## Stage 2 on `a100` (du04) — IBGDA works, and the design changes

MN5 was the wrong machine. `a100` (`du04`, reachable via `university-base`) has
4× A100-SXM4-40GB and one ConnectX-5 (fw 16.35.8002, port ACTIVE, InfiniBand),
and — unlike MN5 — it is **configured** for GPU-initiated RDMA:

| | MN5 ACC | du04 |
|---|---|---|
| `nvidia_peermem` | NOT LOADED | loaded |
| `PeerMappingOverride` | absent | `=1` |
| NVSHMEM device check | *"no active IB device that supports GPU-initiated communication"* | `mlx5_0` accepted |

Those two driver settings are IBGDA's documented requirements, which makes MN5's
failure look like **configuration rather than incapable hardware** — worth a BSC
ticket rather than writing the cluster off.

**IBGDA runs here**, confirmed end to end: `nvshmem put-ring OK: 2 PEs, 1024
ints, 2 iters`, exit 0, with `NVSHMEM_REMOTE_TRANSPORT=ibgda` pinned so a
fallback would have failed the run outright. Single node is not an obstacle:
`NVSHMEM_DISABLE_P2P=1` forces the remote IB transport between two local PEs, so
the NIC is exercised in loopback. That is enough to observe doorbells and WQEs.
It is *not* enough for any bandwidth claim — CX-5 on 40 Gb FDR10.

### Four findings, in increasing order of consequence

1. **NVSHMEM 3.4.5's IBGDA calls `mlx5dv_devx_umem_reg_ex` exclusively.**
   Measured `reg=0 reg_ex=10`. The legacy `mlx5dv_devx_umem_reg` is never
   called, so the double-counting that `ibv_reg_mr`/`ibv_reg_mr_iova2` caused in
   case 01 does not arise here — but only the `_ex` hook sees anything at all.
   The first run on du04 logged **zero** umem lines while creating six DEVX queue
   pairs, purely because the probe hooked the older symbol.

2. **The probe broke the application before it observed it.** `mlx5dv_*` is
   exported by `libmlx5.so.1`, which rdma-core `dlopen`s as a provider plugin —
   and `dlopen`'d objects are not in the `RTLD_NEXT` chain. Every lookup returned
   null, the interposers dutifully returned `nullptr`, and NVSHMEM read that as a
   failed UAR allocation and disabled the whole transport:

   ```
   ibgda.cpp:1830: NULL value cannot allocate mlx5dv_devx_uar
   WARN: GPU cannot map UAR of device mlx5_0. Skipping...
   ```

   The same workload without the shim runs fine. This is the failure mode the
   plan warns about for option A, arriving early and from the *passive* layer.
   Two rules follow for `providers/mlx5dv/`: resolution must fall back to the
   provider libraries by name (`smart_dlsym` needs to handle `dlopen`'d plugins),
   and a hook that cannot find its real function must **abort loudly**, never
   synthesise a failure the application did not cause.

3. **IBGDA registers NIC buffers by dmabuf fd, not by address.** Every
   registration is `addr=0x0 dmabuf=85`. So SigmaTrace's discriminator —
   `cudaPointerGetAttributes` on the umem address to decide GPU residency
   (`Mlx5dv.cpp:82-95`) — **cannot work on this NVSHMEM/rdma-core combination**.
   There is no pointer to classify. Any port of that code would silently decide
   "not a GPU buffer" and skip every IBGDA queue pair, which is precisely the
   quiet-wrong-answer failure this whole line of work exists to prevent.

4. **`cuMemGetHandleForAddressRange` is invisible to `LD_PRELOAD`.** That call
   mints the dmabuf fd *from a GPU virtual address range*, so it is the one place
   the `fd → [base, base+size)` mapping exists — and hooking it produced **zero**
   hits, meaning NVSHMEM reaches it through `cuGetProcAddress`.

### What (4) changes

The CUDA export-table interception subsystem
(`Profiler/Modules/CudaExportTableOverwrite/`) was scoped in this plan as
option A's cost — the price of the UAR hijack, and a reason to prefer B. That is
now wrong. **Both options need it**, because the GPU-memory↔NIC binding itself
travels through the driver API:

- Option B needs it to learn where the send queues *are* before NVBit's addresses
  can be joined against anything.
- Option A needs it for that and for the UAR substitution.

So "B avoids the scary driver patching" is not a real advantage, and the cost
comparison collapses back to the one the user made at the outset: the hijack is
cheaper at runtime than instrumenting every global memory instruction. What
survives of B's case is only the marginal-cost argument — free-ish on a capture
already running NVBit, and passive.

### Next

- Recover `fd → GPU VA` despite `cuGetProcAddress`. Cheapest first step is a
  `cuGetProcAddress` interposer that only *logs* which symbols are requested,
  to confirm the route before building anything that patches a table.
- Then Stage 2's actual question is answerable: join NVBit's addresses against
  the send-queue ranges.
- Stage 1 (`providers/mlx5dv/`) is unaffected and still the right first
  deliverable — with finding (1) fixing which symbol to hook, and (2) and (3)
  fixing how.

## The routing question, answered: NVSHMEM hides CUDA behind `dlsym`

Hooking `cuMemGetHandleForAddressRange` gave zero hits, so the next step was a
**log-only** interposer on `cuGetProcAddress_v2` and `cuGetExportTable` —
observing which entry points get requested, deliberately not replacing any
pointer, because replacing driver entry points is the change that had already
broken the transport once.

Both came back **zero**, which is too strange to be a finding and had to be a
probe fault. It was, and the object files say why:

```
nvshmem_transport_ibgda.so : cu* symbols imported = 0   (links no CUDA at all)
libnvshmem_host.so.3       : cu* symbols imported = 0
                             dl* imports          = dlopen, dlsym, dlclose
                             strings              = libcuda.so.1,
                                                    cuGetProcAddress,
                                                    cuGetProcAddress_v2
```

NVSHMEM `dlopen`s `libcuda.so.1` and pulls `cuGetProcAddress` out of it with
`dlsym(handle, …)`, then resolves everything else through that. **A
handle-scoped `dlsym` searches that library directly and never consults the
global scope, so `LD_PRELOAD` cannot intercept any of it.** No additional
preloaded symbol would have helped; the interposition point has to be `dlsym`
itself.

Which is exactly why SigmaTrace carries a `SmartDlsym` module, and — the good
news — **pcomm already has one**: `libpcomm/src/hook/Dlsym.cpp:107` interposes
`dlsym`, and `:136` `dlvsym`, with `PreloadHookManager` already resolving
through `real_dlsym`. The probe could not see any of this only because it is a
standalone shim that links nothing from libpcomm, on purpose.

### Consequences

1. **The export-table patcher may not be needed at all.** It was scoped as
   option A's cost and then, a section ago, as a cost both options share. The
   actual route here is `dlsym → cuGetProcAddress`, and pcomm can already
   intercept the first hop. SigmaTrace patches `cuGetExportTable` *as well*, but
   nothing observed on du04 requires it. That should be established, not
   assumed — but it moves the most invasive component in the reference design
   from "required" to "unproven necessity".
2. **Stage 1 is unaffected and confirmed working.** Every mlx5dv hook fired
   under plain `LD_PRELOAD` — `umem_ex`, `uar`, and `devx` lines all captured on
   a live IBGDA run. The DEVX control plane needs no driver interception at all.
3. **The dmabuf finding is confirmed, not an artefact.** `comp_mask=0x1` is
   `MLX5DV_UMEM_MASK_DMABUF`, so `dmabuf=85/86` are genuine descriptors and the
   `addr=0x0` registrations are real. SigmaTrace's address-based GPU-residency
   test still cannot work here.

### Revised next steps

- Build Stage 1 (`providers/mlx5dv/`) using pcomm's existing preload machinery.
  It is unblocked, needs nothing new, and is now demonstrated against real IBGDA
  traffic rather than argued for.
- For the send-queue addresses, register `cuMemGetHandleForAddressRange` with
  `smart_dlsym` rather than exporting a symbol — that is the interception point
  the evidence actually points at.
- Only if that proves insufficient, revisit `cuGetExportTable`.

## Correction: `cuGetExportTable` IS used — walking back the previous section

The section above concluded that the export-table patcher was an "unproven
necessity" because a log-only interposer saw zero calls. That reasoning was
sound as far as it went — `LD_PRELOAD` cannot see dlsym-on-handle — but the
conclusion drawn from it was too strong. `LD_DEBUG=symbols` on du04 shows what
NVSHMEM actually resolves:

```
cuGetProcAddress               : 2
cuGetProcAddress_v2            : 8
cuGetExportTable               : 2
cuMemHostRegister_v2           : 2
cuMemHostGetDevicePointer_v2   : 0
cuMemGetHandleForAddressRange  : 0
```

Both of SigmaTrace's interception points are live. Its infrastructure choice —
patch `cuGetProcAddress` *and* `cuGetExportTable`, because CUDA hands out entry
points through both doors — is **validated by this evidence**, not made
redundant by it. Treat the previous section's "unproven necessity" as retracted.

(The first measurement of this printed zeros for every symbol including
`cuMemHostRegister`, because the grep matched `lookup symbol.*NAME` while
`LD_DEBUG` writes `symbol=NAME`. The sanity line in the same output contradicted
the table, which is the only reason it was caught. Counted patterns that return
zero need a positive control every time.)

### What remains genuinely open

`cuMemHostGetDevicePointer_v2` shows **0 lookups**, and it is the linchpin of the
UAR hijack: SigmaTrace substitutes the fake page by returning a device pointer
for it (`CUDAHijack.cpp:467-519`). Two readings, and this data cannot separate
them:

1. It is never called — plausible, since under unified addressing a
   `CU_MEMHOSTREGISTER_DEVICEMAP` registration makes the host VA usable on the
   device directly, so there is nothing to ask for. If so, **the substitution
   point does not exist on this stack** and the hijack needs a different hook.
2. It *is* called, via `cuGetProcAddress("cuMemHostGetDevicePointer")` — which
   takes a **string**, not a dynamic symbol lookup, and is therefore invisible to
   `LD_DEBUG` by construction. The 8 `cuGetProcAddress_v2` lookups say that route
   is in use for something.

Settling it requires intercepting `cuGetProcAddress` itself and logging the
strings — which requires interposing `dlsym`, which the standalone probe does not
do and `libpcomm` already does (`Dlsym.cpp:107`). So this question is now blocked
on doing it inside pcomm rather than beside it, which is an argument for building
Stage 1 next rather than extending the probe further.

### Standing assessment of SigmaTrace

- **Mechanism**: sound, and its interception infrastructure is confirmed
  relevant on current hardware.
- **Host repeater path**: complete, coherent code. Never executed here — "it
  works" remains code-reading, not evidence.
- **GPU-resident repeater**: definitely not working code (unreachable parse,
  debug `printf`).
- **NVSHMEM bindings**: measurably stale for 3.4.5 — hooks `umem_reg` where only
  `umem_reg_ex` is called, and decides GPU residency by an address that is now
  always `0x0` under dmabuf registration. A verbatim port captures nothing, and
  fails silently while doing so.

## SigmaTrace was built and run on du04. It crashes the application.

Enough theorising — it was built from source on du04 and pointed at the same
IBGDA workload. Every build blocker was environmental, none in SigmaTrace:
cmake 3.31.6 via pip (not 4.x, which drops the `<3.5` minimums Boost and
protobuf still declare), abseil and protobuf shallow-cloned at their pinned
tags, system `libboost-program-options-dev` instead of Boost's superproject
(only `program_options` is needed, no minimum version), and
`cuda-cupti-dev-13-0`. Built on `/DATA` (177 GB free), never `/` (98% full, and
other people's jobs live there). **Zero compile errors**, and the configure
found what matters:

```
RDMA-core: ON  (ibverbs + mlx5)      target/providers/libmlx5dv_trace.so
                                     target/providers/libibverbs_trace.so
                                     target/lib/libsmart_dlsym.so
```

### Observed

| run | result |
|---|---|
| `putget`, 2 PEs, no profiler | `put-ring OK`, exit 0 |
| `Profiler --enable-doorbell-hijack`, **1 PE** | `Exited Normally!`, 0 doorbells |
| `Profiler --enable-doorbell-hijack`, **2 PEs** | **abort, exit 6**, 0 doorbells |

```
[Profiler] loaded mlx5dv Trace
[Profiler] Loaded Doorbell Hijack Trace
terminate called after throwing an instance of 'std::out_of_range'
  what():  absl::container_internal::raw_hash_map<>::at
[Profiler] num_nic_doorbell_accesses: 0
[Profiler] doorbell_submitted: 0
```

The same workload runs clean without the profiler, so SigmaTrace causes the
failure. One PE does no remote communication and survives; two PEs generate real
device-initiated traffic and abort. The crash is on the traffic path.

### Diagnosis — high confidence, but NOT backtrace-confirmed

The throwing `.at()` is almost certainly `m_mlx5_qp_info.at(dbr_qpn)` in
`on_doorbell_ring` (`CUDAHijack.cpp:394`), whose own comment reads
*"// Should be guaranteed to exist"*. Two independent legs support it:

- **Symptom shape.** It fires only when doorbells ring (2 PEs), never without
  (1 PE).
- **The map cannot be populated on this stack**, measured independently by our
  own probe before SigmaTrace was ever built: NVSHMEM 3.4.5 calls only
  `mlx5dv_devx_umem_reg_ex` (`reg=0 reg_ex=10`) while SigmaTrace hooks
  `mlx5dv_devx_umem_reg`; and its GPU-residency test reads the umem address,
  which is `0x0` under dmabuf registration (`comp_mask=0x1`). `Mlx5dv.cpp:82-87`
  then returns early and no `QpInfo` is ever inserted. So the *first* doorbell
  throws.

Not confirmed by a stack frame: gdb on the launcher misses the crash (Profiler
forks and execs the app), and gdb on the app breaks MPI's init handshake. Worth
one more attempt before this is treated as settled.

### What this actually tells us

1. **The interception infrastructure works.** A doorbell reached
   `on_doorbell_ring` at all, which means the UAR substitution engaged — so the
   export-table/`cuGetProcAddress` patching does its job on current CUDA, and
   `cuMemHostGetDevicePointer` *is* reached that way (invisible to `LD_DEBUG`,
   exactly as flagged). The mechanism is sound and validated.
2. **The NVSHMEM bindings are stale**, and predicted to be so from reading the
   code before running it — same two defects, now demonstrated.
3. **It fails destructively, not silently.** The earlier prediction was "captures
   nothing"; the truth is worse. An unregistered QP is met with `.at()` rather
   than a lookup-and-skip, so the profiler aborts the application on its first
   device-initiated send. For pcomm this is the sharpest possible restatement of
   the rule already adopted for the probe: **a hook that meets something it does
   not recognise must degrade, never terminate.**

### Bearing on the plan

Port the *mechanism*, not the code. `providers/mlx5dv/` must hook `umem_reg_ex`,
identify GPU buffers by dmabuf handle rather than address, and treat an unknown
QP as a record to drop rather than a precondition to assert.

## RETRACTION: SigmaTrace works. The crash was my configuration.

The paper's artifact description settles it, and the fault was mine.

Its A2/A4 platform is *"Single node, 4× NVIDIA A100-SXM4-40GB, 1× Mellanox
ConnectX-5 (MT27800)"* — du04 exactly. Single-node was never the problem; it is
the machine the doorbell benchmarks were published on. But the environment they
use is not the one I used:

```
NVSHMEM_REMOTE_TRANSPORT=ibdevx      I used: ibgda
NVSHMEM_IB_DISABLE_DMABUF=1          I used: unset (deliberately removed)
```

Re-run with their configuration, unchanged binary:

```
nvshmem put-ring OK: 2 PEs
[Profiler] num_fake_nic_accesses: 1030   (rank 1: 1033)
[Profiler] doorbell_submitted:    23
[Profiler] doorbell_skipped:      1007
[Profiler] Exited Normally!
```

Over a thousand intercepted doorbells per rank, application completes, exit 0.
**SigmaTrace's UAR interposition works on current hardware, on this NVSHMEM
(3.4.5, newer than the paper's 3.3.24), and is not version-stale.**

### How the error was made

`NVSHMEM_IB_DISABLE_DMABUF=1` was removed two sections ago, and the reasoning
was recorded as *"an environment flag carried across clusters without checking
which half of an either/or it disables"*. That reasoning was correct **for MN5**,
where no peermem module exists and DMABUF is the only route to GPU memory.
It is wrong for du04, which has `nvidia_peermem` loaded — there, disabling
DMABUF pushes registration onto the peermem path, where umems carry **real
addresses** instead of `addr=0x0 + dmabuf fd`.

And a real address is precisely what SigmaTrace's GPU-residency discriminator
needs. So the chain was:

> removed the flag → registrations became dmabuf → `cudaPointerGetAttributes`
> could not classify them → `Mlx5dv.cpp:82-87` returned early → `m_mlx5_qp_info`
> stayed empty → first doorbell hit `.at()` → abort.

The mechanical diagnosis was right — the QP map *was* empty and `.at()` *did*
throw. The attribution was wrong. I called it version rot in SigmaTrace when it
was misconfiguration by me, then read the resulting crash as confirmation of a
prediction I had already published in this file. Predicting a failure and then
causing it is not evidence.

### What stands and what falls

**Falls:**
- "A verbatim port captures nothing" — false in the supported configuration.
- "The NVSHMEM bindings are measurably stale" — false; they work against a
  *newer* NVSHMEM than the paper targets.
- "It fails destructively" — it fails destructively *when driven outside its
  supported configuration*, which is a much weaker and different claim.

**Stands:**
- The mechanism is validated, now by execution rather than by reading.
- `.at()` on an unregistered QP does abort the application rather than skip the
  record. Still worth not copying: pcomm should degrade, not terminate, when it
  meets something it does not recognise. This is now a robustness note about an
  unsupported configuration, not a defect claim.
- Under `REMOTE_TRANSPORT=ibgda` + DMABUF enabled, registrations really are
  `addr=0x0` with `comp_mask=0x1`, and `umem_reg_ex` really is the only entry
  point called. Those measurements were independent and remain true — they
  describe a configuration SigmaTrace does not claim to support, and are a
  coverage question for pcomm rather than a bug in SigmaTrace.

### Bearing on the plan

Port the mechanism with confidence — it is demonstrated working on the exact
hardware available. Two additions to Stage 1:

1. **Record the transport configuration in the capture.** A capture taken under
   `ibgda`+dmabuf and one taken under `ibdevx`+peermem are not the same
   experiment, and this session lost hours to not distinguishing them.
2. **Support both registration paths** — address-based (peermem) and
   dmabuf-fd-based — rather than only the one the reference implementation
   needs. That is where pcomm can genuinely exceed the prior art.

## What actually shipped (2026-08-04/05)

Stage 1 is done, and several of this plan's own assumptions were corrected on
the way by hardware rather than by argument.

**`providers/mlx5dv/`** — ids 200-299, reserved in `ids.hpp` since it was
written and unused until now. Records: `mlx5_umem`, `mlx5_uar`,
`mlx5_devx_cmd`, `mlx5_devx_qp`, `mlx5_devx_qp_connect`, `mlx5_queue_pair`.
Hooks: `devx_umem_reg`, `devx_umem_reg_ex`, `devx_umem_dereg`,
`devx_alloc_uar`, `devx_free_uar`, `devx_obj_create`, `devx_obj_modify`,
`create_qp`. Registered in the launcher as `devx` (aliases `mlx5`, `mlx5dv`,
`ibgda`), non-exclusive so it co-runs with `ib`.

**`tests/hpc/cases/09_ibgda_putget`** — assertions only, no goldens, because
every quantity here is allocation-dependent. `verify_asserts.sh` tests the
assertions themselves against a synthetic fixture (22 checks) and needs only
sqlite3, so they are exercised on hosts that cannot run IBGDA at all.
`tests/hpc/run_on_a100.sh` is the runner; MN5 self-skips with a reason.

**Corrections this plan owes to measurement**, each of which was first argued
the other way in the sections above:

| claimed earlier | what hardware said |
|---|---|
| the export-table patcher is "unproven necessity" | `cuGetProcAddress` 2, `_v2` 8, `cuGetExportTable` 2 — SigmaTrace's design is validated |
| SigmaTrace is "version-stale" and captures nothing | it works; I ran it in the configuration it does not support and read the crash as confirmation of my own prediction |
| src_lid decodes from INIT2RTR | it does not — a queue pair is bound to a port at RST2INIT, which `ib_records.hpp` already documented for the verbs path |

**Engine changes this required:** void-returning hooks (`HookSigs` void
specialisation + `is_void_v` branch in `invoke_preload`) — without them no
provider could hook a symbol returning void, which is why `free_uar` was
initially dropped.

### What Stage 3 now looks like

The A-vs-B question is settled on cost and re-opened on scope. The user's
original point stands: the doorbell hijack is far cheaper at runtime than
instrumenting every global memory instruction, and B's only remaining argument
is marginal cost on a capture already running NVBit.

But **both need CUDA driver interception**, and the route is now known
precisely: NVSHMEM `dlopen`s `libcuda.so.1` and resolves through
`dlsym(handle, …)`, which bypasses `LD_PRELOAD` entirely. pcomm already
interposes `dlsym` (`Dlsym.cpp:107`), so the first hop is covered; whether
`cuGetExportTable` patching is *also* required is the open question, and the
measurement above says SigmaTrace was right to do it.

Prerequisite for any of it: **`fd → GPU VA` for dmabuf-registered send
queues.** Under `ibgda`+dmabuf the umem carries `addr=0x0` and a descriptor, so
`sq_base` is an offset and the send queue has no address until
`cuMemGetHandleForAddressRange` is intercepted through `smart_dlsym`. Nothing
in the data plane can be located without it.

### Follow-ups that are now their own plans

- Case 10, the `ibdevx`+peermem (`by_addr`) configuration — the one SigmaTrace's
  artifact uses, and the half of `umem_referential` currently only reasoned
  about.
- Entity close records (`PLAN-20260805-0053-entity-lifetimes-todo.md`).
- Case 08's unaccounted queue pair
  (`PLAN-20260805-0053-perftest-qp-creation-todo.md`).
