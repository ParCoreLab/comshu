# PLAN — the IBGDA data plane: doorbell hijack, to SigmaTrace parity

- **Created:** 2026-08-07 17:46 +03
- **Status:** TODO. This is stages 2-4 of
  `PLAN-20260803-2253-ibgda-device-initiated-capture-in-progress`, written out
  now that stage 1 (the DEVX control plane) has shipped and the A-vs-B question
  is settled on measurement rather than argument. That plan stays the parent;
  this one is the implementation it deferred.
- **Prior art:** `../SigmaTrace` —
  `Profiler/Modules/MemoryHijack/` (CPU repeater, 169 lines),
  `Profiler/Modules/GPUMemoryHijack/` (GPU repeater, 770 lines),
  `Profiler/Modules/CudaExportTableOverwrite/` (driver interception, 221 lines),
  `Profiler/Handlers/CUDAHijack.cpp:336-590` (the UAR page swap that ties them
  together).
- **Related:** `PLAN-20260806-2140-dmabuf-registered-regions-done` — the dmabuf
  descriptor is why `sq_base` currently has no address.

## What is broken

pcomm's `devx` provider captures IBGDA's **control** plane: umems, UARs, DEVX
queue pairs, their connect commands. It captures none of the traffic. In IBGDA a
device thread builds the WQE in a send queue that lives in GPU HBM and rings a
doorbell mapped into the GPU's address space:

```c
nvshmemi_ibgda_device_qp_t *qp, uint16_t prod_idx) {
    uint64_t *bf_ptr = (uint64_t *)qp->tx_wq.bf;
    ibgda_ctrl_seg_t ctrl_seg = { .opmod_idx_opcode = HTOBE32(prod_idx << 8),
                                  .qpn_ds          = HTOBE32(qp->qpn << 8) };
    ibgda_store_release(bf_ptr, *((uint64_t *)&ctrl_seg));
}
```

One GPU store to a BAR. The CPU is never involved, so there is nothing to
interpose and a GPU-initiated run reports queue pairs that never send.

## The mechanism, in the order the pieces have to exist

SigmaTrace's design is a **man-in-the-middle on the doorbell page**, and the
whole thing hangs off one substitution: give the GPU a *fake* page to ring, watch
it, then forward the write to the real UAR.

```
  mlx5dv_devx_alloc_uar(ctx)                -> we already record this (uar addr)
  cuMemHostRegister(uar, DEVICEMAP)         -> remember: this page is a UAR
  cuMemHostGetDevicePointer(&dptr, uar)     -> THE SWAP. allocate a fake page,
                                               return the fake page's device ptr
  ... GPU rings the fake page ...
  repeater sees the store, decodes ctrl seg -> qpn, prod_idx
  reads the WQEs out of the SQ at prod_idx  -> opcode, laddr/lkey, raddr/rkey, len
  forwards the store to the real UAR        -> the NIC sends, application unharmed
```

The decode is `struct mlx5_wqe_ctrl_seg`: `opmod_idx_opcode` carries `prod_idx <<
8`, `qpn_ds` carries `qpn << 8`, both big-endian. The WQE itself is at
`sq_base + (prod_idx & ((1 << log_wq_size) - 1)) * 64`.

**Read the WQE before forwarding.** Once the NIC consumes the doorbell the GPU
is free to recycle that WQEBB, so the ordering in SigmaTrace's repeater loop —
callback first, store second — is load-bearing and not stylistic.

## Stages

### Stage 0 — prerequisites, both already named by the parent plan

**0a. `fd → GPU VA` for dmabuf-registered send queues.** Under `ibgda`+dmabuf the
umem carries `addr=0x0` and a descriptor, so `sq_base` is an offset into an
object with no address. Without this the repeater knows a doorbell rang and
cannot read the WQE it announced. `cuMemGetHandleForAddressRange` through
`smart_dlsym` is the interception; `core/dmabuf_registry.hpp` is where the answer
belongs, since ibverbs already consults it.

**0b. CUDA driver interception.** NVSHMEM `dlopen`s `libcuda.so.1` and resolves
through `dlsym(handle, …)`, which bypasses `LD_PRELOAD`. pcomm already interposes
`dlsym` in the engine, so the first hop is covered — but du04 measured
`cuGetProcAddress` 2 calls, `cuGetProcAddress_v2` 8, `cuGetExportTable` 2, so the
export-table route is *used* and SigmaTrace was right to patch it. Port
`ExportTablePreload.cpp`: a name → replacement registry, `cuGetExportTable` and
`cuGetProcAddress_v2` interposers that hand back our function and keep the real
one.

This is engine-level, not provider-level — it belongs beside `Dlsym`, because the
gpumem provider will want the same door for `cuMemAlloc*`/`cuMemFree*`.

### Stage 1 — the swap (no recording yet)

Register three overwrites: `cuMemHostRegister_v2` (remember DEVICEMAP'd pages),
`cuMemHostGetDevicePointer_v2` (the swap), `cuMemHostUnregister` (teardown).

Swap only pages that `mlx5dv_devx_alloc_uar` gave out — the provider already has
that table. Everything else passes straight through, untouched, which keeps the
blast radius to processes doing IBGDA.

Ship this stage with the repeater as a **pure forwarder**: fake page in, real UAR
out, no decode, no records. If a full NVSHMEM run is byte-identical and no slower
than the mean of three clean runs, the man-in-the-middle is sound and everything
after it is decode. If it is not, stop here — no amount of correct parsing
rescues a data path that perturbs the application.

### Stage 2 — CPU repeater + records

Port `MemoryRepeater`: one thread per hijacked page, parked in
`UMONITOR`/`UMWAIT` (WAITPKG — MN5's Sapphire Rapids has it; `MONITORX`/`MWAITX`
is the AMD path; `_mm_pause` is the fallback, and on that fallback this is a spin
loop per UAR page and the cost must be stated in the record, not hidden).

Records, ids in the mlx5dv range (200-299; `200 202 204 206 208 210` taken):

- **`Mlx5Doorbell`** (Event, 212) — `uar`, `qp_num`, `prod_idx`. What was rung
  and when. Emitted even when the WQE cannot be read, so "the doorbell rang and
  we could not decode it" is a visible row and not silence. That distinction is
  the entire lesson of the last three weeks.
- **`Mlx5DeviceSend`** (Event, 214) — `qp_num`, `wqe_idx`, `opcode`, `laddr`,
  `lkey`, `raddr`, `rkey`, `length`. The GPU-initiated counterpart of
  `ib_post_send`, and deliberately the same shape so a reader can union them.

### Stage 3 — GPU repeater

The CPU repeater adds a PCIe round trip to every doorbell. When that perturbs the
workload past the stage-1 budget, the answer is SigmaTrace's persistent kernel:
poll the fake UAR addresses on-GPU, parse the WQE there, push a `GpuWqeRecord`
into a pinned ring the host drains, forward to the real BAR pointer.

SigmaTrace runs it in a **green context with one SM** so the profiler's kernel
does not contend with the application's. Port that, not a plain persistent
kernel; without it this steals an SM from the workload it is measuring.

Treat stage 3 as *conditional on stage 2's measured overhead*. If the CPU
repeater is inside budget on the cases we have, do not carry 770 lines of CUDA
for a problem this hardware does not have — and say so in the plan's close, with
the number.

## What makes this different from every other pcomm mechanism

**It sits in the data path.** Every hook in this tree so far is passive: if it
breaks, a table is empty. This one owns the doorbell — if the forwarding store is
dropped, reordered, or late, the application hangs or corrupts a transfer, and
the failure appears in the *application*, not in the capture.

That earns rules the rest of the tree does not need:

- **Forward unconditionally.** The record path must never be able to skip the
  store. Callback first, store second, and the store is not inside any branch
  that a parse failure can take.
- **Fail open, not closed.** Cannot allocate the fake page, cannot find the SQ,
  slot table full → return the *real* device pointer and record that this UAR is
  unwatched. Slot exhaustion already taught this once: ibverbs' 256 contexts
  aborted the traced application until it was made to degrade.
- **Never manufacture a capability.** The `devx_probe` mistake and the
  `verbs_context` NULL-slot guard are the same lesson: if the application did not
  device-map this page, we do not make it look device-mappable.
- **GPU→PCIe→NIC ordering is not CPU ordering.** SigmaTrace's own comment on the
  acquire load says "this is possibly still not good", which is an open question
  inherited with the code, not a solved one. Whatever ordering we pick needs a
  written justification against the PRM, not `__ATOMIC_ACQUIRE` because it
  compiled.

## Verification, and the scheduling fact that shapes it

**MN5 cannot verify any of this.** Cases 09/10 SKIP there for want of
`PeerMappingOverride=1` and 11 for want of `nvidia_peermem`. The whole plan is
verifiable only on **du04**, through `tests/hpc/run_on_a100.sh` — so this work
does not get a green board from the usual runner, and any claim that it works has
to name a du04 job.

Gates, in order:

1. Stage 1: full NVSHMEM run, unmodified output, overhead inside the noise of
   three clean runs. The forwarder is correct or nothing else matters.
2. Stage 2: `Mlx5DeviceSend` count equals the application's own reported message
   count on a put/get microbenchmark where that number is known by construction —
   case 09's workload already is one.
3. Every `Mlx5Doorbell` names a `mlx5_devx_qp` some record created. The
   close-accounting assertion generalises to it unchanged, and an unmatched
   doorbell means a QP creation door is missing — the same detector, pointed at
   the device plane.
4. A negative control: a run with the hijack disabled must produce zero doorbell
   rows and identical application output. "It captured nothing" and "there was
   nothing to capture" have to stay distinguishable.

## Rejected

**Instrumenting the doorbell store with NVBit** (option B in the parent plan).
Cheaper to *build* — the gpumem provider already instruments global stores — and
far more expensive to run: it pays instrumentation on every global memory
instruction to catch the handful that are doorbells. The parent plan settled this
on cost, and both options need the same CUDA driver interception anyway, so B
saves nothing that stage 0 does not already have to build.

**Reading the SQ from the CPU on a timer**, without hijacking the doorbell. No
doorbell means no `prod_idx`, so there is no way to know which WQEs are new, and
the GPU recycles WQEBBs underneath the reader. It would produce plausible rows
that are silently wrong, which is worse than an empty table.
