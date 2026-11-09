# PLAN — CPU-assisted IBGDA: the data plane MN5 can actually give us

> **Closed 2026-11-09 (backlog triage).** Built as providers/mlx5dv/host_doorbell.hpp (`--enable-host-doorbell`); case 12 (NVSHMEM) and case 26 (NCCL GIN) capture their whole data plane through it on MN5. The status below is historical.

- **Created:** 2026-09-05 12:24 +03
- **Status:** TODO. Design settled from source; nothing built.
- **Parent:** `PLAN-20260807-1746-ibgda-data-plane-hijack-done` — this plan does
  not replace the hijack. It covers the mode the hijack structurally cannot
  reach, which happens to be the only mode our largest cluster runs.
- **Sibling:** `PLAN-20260807-2344-ibgda-on-gpu-repeater-backlog` — Stage 3. Still
  the right answer on du04. Useless on MN5, for the reason below.
- **Evidence:** `reference/papers/NSDI_27______Dissecting_GPU_Initiated_Networking.pdf`
  §4.1 (edge case), §6.1 (platform table), and
  `reference/Dissecting-IBGDA-Experiments/` — the paper's own harness, which
  runs on MN5 and records what it had to do there.

## The fact this plan exists for

**MN5 is the paper's P-H100, and full GPU-submitted IBGDA does not work on it.**

From `reference/Dissecting-IBGDA-Experiments/env/nvshmem345_mn5.sh`:

> Full IBGDA (NIC_HANDLER=gpu) does NOT work here: the open kernel module has no
> PeerMappingOverride, cudaHostRegister(IoMemory) on the NIC UAR returns
> cudaErrorNotPermitted, and NVSHMEM silently falls back to the CPU handler at
> exit code 0.

and `tier-a-experiments.md`: *"P-H100: large H100 + CX7 cluster. No
GPU-submitted IBGDA. Proxy (IBRC) and CPU-assisted IBGDA only."*

pcomm's doorbell hijack hangs off exactly the call that fails there. Its whole
mechanism is: recognise the UAR at `cuMemHostRegister(…, DEVICEMAP)`, swap the
device pointer at `cuMemHostGetDevicePointer`, park a repeater on the fake page.
On MN5 the register call returns `cudaErrorNotPermitted`, so there is no device
pointer to swap, no page to watch, and `mlx5_device_send` is empty. It is empty
for a platform reason, and it will stay empty no matter how good the repeater
gets — including the on-GPU one, which forwards a GPU store to a BAR page that
this driver will not map into a GPU at all.

So the standing plan for the IBGDA data plane covers du04 and the paper's P-IB,
and covers nothing on the machine every HPC case in `tests/hpc/` runs on.

(The `report_and_shutdown` guard added 2026-09-05 makes this *visible* — DEVX
UARs allocated, none mapped into the GPU, therefore the doorbell writer is not
the GPU. It does not make it *captured*. That is this plan.)

## Why this mode is easier to capture, not harder

In CPU-assisted mode the GPU still does everything interesting: it builds every
WQE in a GPU-resident send queue and advances a producer index. Only the
doorbell moves to the host. From
`reference/nvshmem-3.7.2-0/src/modules/transport/ibgda/ibgda.cpp`
(`ibgda_rc_progress`, and `ibgda_dci_progress` in the same shape):

```c
prod_idx_buffer = (uint64_t *)ep->qp_ctrl.prod_idx_mobject->aligned.cpu_ptr;
...
*prod_idx_snapshot = ibgda_read_once(prod_idx_buffer);      // or gdrcopy_ftable.copy_from_mapping
if (*prod_idx_cache < *prod_idx_snapshot) {
    dbrec = (__be32 *)(ep->qp_ctrl.dbr_mobject->aligned.cpu_ptr + ...);
    bf    = (__be64 *)ep->uar_mobject->aligned.cpu_ptr;
    ctrl_seg.qpn_ds          = htobe32(ep->qpn << 8);
    ctrl_seg.opmod_idx_opcode = htobe32(*prod_idx_snapshot << 8);
    ibgda_write_once(dbrec, htobe32(*prod_idx_snapshot & 0xffff));
    std::atomic_thread_fence(std::memory_order_release);
    ibgda_write_once(bf, *((__be64 *)&ctrl_seg));            // <- the doorbell, on the HOST
    *prod_idx_cache = *prod_idx_snapshot;
}
```

Three properties make this a better capture surface than the GPU path:

1. **The doorbell is a host store.** No BAR mapping, no fake page, no repeater
   thread of ours, no host round trip added to the application's critical path.
   The CPU is already on this path; NVSHMEM put it there.
2. **`prod_idx` is a full 64-bit monotonic counter**, not the 16-bit wrapped
   index the doorbell register carries. The lapping ambiguity that
   `wqes_lapped` exists for does not arise: `[cache, snapshot)` is exact, and a
   reader that misses a poll loses nothing but timing resolution.
3. **The WQEs are still GPU-resident and still ours to decode.** `copy_wqes` and
   `parse_wqes` in `uar_hijack.hpp` are unchanged — same send queue, same
   `sq_base` from the DEVX `CREATE_QP` decode, same segment layout.

The mechanism this plan needs is therefore *only* a trigger. Everything
downstream already exists and is now correct for both AV widths.

## The open question: which door

Not settled, and it must be settled by measurement rather than by reading, which
is this provider's standing rule and the one that cost case 08 a week.

- **(a) `nvshmemt_ibgda_progress` / `ibgda_rc_progress` / `ibgda_dci_progress`.**
  The transport is a dlopened plugin (`nvshmem_transport_ibgda.so.N`). Whether
  these are exported dynamic symbols is a `readelf` question, not a source
  question — NVSHMEM resolves `nvshmemt_init` by name and reaches the rest
  through function pointers in the transport struct, which says nothing about
  visibility. **Check `readelf --dyn-syms` on the built plugin on MN5 first.**
  If exported, a plain preload hook wraps it, and the post-hook reads the
  per-EP `prod_idx_cache` deltas the call just committed. Cheapest possible
  answer, and version-fragile in the way every internal-symbol hook is.
- **(b) The UAR's host mapping.** pcomm already owns `mlx5dv_devx_alloc_uar`
  and therefore `reg_addr`. Substituting a decoy host page in the returned
  struct and running the existing repeater on it needs no NVSHMEM symbol at all
  and works for any CPU-assisted submitter, not just NVSHMEM's. It costs a
  poll thread per UAR — but host→host, with no PCIe round trip and no
  `cuMemcpyDtoHAsync` unless the WQE decode is on. Watch for: NVSHMEM writes
  `dbrec` before `bf` with a release fence between, and forwarding must preserve
  that order.
- **(c) Neither: poll the producer-index buffers directly.** The most faithful
  option — zero writes on the application's path, we only read — and the one
  with no address to key on, since `prod_idx_mobject` is an NVSHMEM-internal
  allocation. Would need it recovered from the umem registrations pcomm already
  records, which is a real inference and may not be unique.

Recommendation: **check (a)'s symbol table, fall back to (b).** (b) is the one
that generalises past NVSHMEM, and generalising is the point — the paper's §6.3
proxies (UCCL-EP, MSCCL++, fabric-lib, NCCL GIN Proxy) all ring doorbells from
host threads, and (b) sees all of them for free.

## What lands when it works

- `mlx5_device_send` and `mlx5_doorbell` populated on MN5 for the first time,
  from a run whose GPU path is untouched.
- The paper's §6.5.3 result becomes reproducible from a pcomm capture rather
  than from a bespoke harness: **active contexts per NIC per time window**,
  which is a query over `mlx5_device_send`+`mlx5_devx_qp` we can already write.
  The knee is at ~1,350 on CX-7 and MN5 is where it was measured. Created QPs
  do not move it; the active set does; pcomm records both. This is a panel, not
  a capture change, and it can be built and tested against the paper's own
  numbers before any of the above lands.
- A stated overhead. On MN5 the doorbell rate is already host-bound
  (55.2 M msg/s CPU-assisted vs 3.98 for IBRC, from
  `results/axisICM_mn5/a2_RESULTS.md`), so the WQE copy-out is the cost to
  measure, and `PCOMM_HIJACK_NO_DECODE` already separates forward from decode.

## What this plan does NOT close

Completion. The CQ stays GPU-resident and the SM still polls it in place, so
nothing here sees a `quiet` — and completion scope is the paper's headline
result (§6.2: PE-wide quiet costs ~0.6 µs per QP, 10.5 → 20.7 → 29.6 µs at
1/16/32 QPs; "how you wait can cost more than how you send"). Two candidate
doors, neither requiring NVBit:

- The CQ's own doorbell record. NVSHMEM's `ibgda_poll_cq` advances a consumer
  index; whether it writes the CQ `dbrec` on every poll, and whether that write
  is host-visible in CPU-assisted mode, is unread and is the first thing to
  check.
- The on-GPU repeater (Stage 3), whose persistent kernel is already in GPU
  memory and could read CQ rings in place. **That plan needs no NVBit** — it is
  a persistent CUDA kernel plus a pinned result ring, not instrumentation. Its
  only blocker is that it is du04-only, for the BAR-mapping reason above.

Until one of those lands, a pcomm capture can say what a run sent and cannot say
what it waited on.
