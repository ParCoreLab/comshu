# PLAN — GPU-initiated sends reach comm_edges but no buffer, device or kernel

- **Created:** 2026-11-09 13:54 +03
- **Status:** DONE (2026-11-09). Phase 1 7755562; Phase 2 29c81e7 + 980f7ee;
  Phase 3 bb41c67 + 0eed85a (recursion fix); Phase 4 measured (bb424ee) and
  shipped as `kernel_sends`. Follow-ups below stay open.
- **Related:** `PLAN-20260806-2140-dmabuf-registered-regions-done` (the fd → VA
  table and the iova rule this extends); `PLAN-20260905-1224-cpu-assisted-ibgda-is-the-cluster-we-have-todo`
  (the host doorbell that produces these rows); case 12 and case 26.

## What is missing, measured

MN5 job 45925495, both cases armed with `--enable-host-doorbell`:

| | case 12 (NVSHMEM IBGDA) | case 26 (NCCL GIN, GDAKI) |
|---|---|---|
| `mlx5_device_send` rows | 675 + 672 | 40,960 per rank, 2,785,280 B |
| `comm_edges` `devx` | exact, 0→1 and 1→0 | exact, 0→1 and 1→0 |
| `object_access` rows from those sends | **0** | **0** |
| sends whose `raddr` lies in a peer `mem_object` | 1,347 of 1,347 | 0 of 81,920 |

So the capture holds the traffic and its endpoints, but it cannot say what
buffer a device-initiated put targeted, which GPU issued it beyond "the rank's
device", or which kernel issued it. Those are three gaps with three causes.

### 1. object_access has no devx branch (reader only)

`object_access` (`pcomm-read/link_sql.hpp`) unions `p2p`, `verbs` and
`verbs_recv`. Nothing reads `mlx5_device_send`. For NVSHMEM the data is already
there: every `raddr` resolves to the peer's symmetric heap object.

### 2. actor_device is the rank's device, not the queue's

The `verbs` branch takes `actor_device` from `_rank_device`. A devx send can do
better: `mlx5_devx_qp.device` is the GPU that owns the queue's send buffer, per
queue (case 12: device 0 on rank 0, device 1 on rank 1, exactly the sending
queues). Use it, and fall back to `_rank_device` only where the queue's device is
unknown (65535).

### 3. NCCL GIN's target has no address, for two independent reasons

- **The fd → VA door is not reached.** GIN converts its window with
  `pfn_cuMemGetHandleForAddressRange`, loaded through
  `cudaGetDriverEntryPointByVersion` from a STATICALLY linked cudart
  (`reference/nccl-git src/misc/cudawrap.cc:172-199`,
  `src/transport/net_ib/gdaki/gin_host_gdaki.cc:104,129`).
  `core/dmabuf_registry.hpp` hooks only `dlsym("cuMemGetHandleForAddressRange")`
  and `dlsym("cuGetProcAddress")`; neither is on that path. The MR is recorded
  with `range_base = 0` (rank 1, 512 KB, `dmabuf_fd = 355`, origin Dmabuf) and
  no `MemObject`.
- **GIN's addresses are zero-based.** It registers with `iova = 0`, so a put's
  `raddr` is an OFFSET into the peer's MR (measured: 0 … 130,944 inside a
  131,072 B window), not a VA. Even with the fd resolved, the reader must add the
  MR's base, which needs the MR found by `rkey` and its `iova` known.
  `IbMemRegion` records `range` (the resolved VA when resolved, else `iova`) but
  not `iova` itself, so after a successful resolve "this MR is zero-based" is
  lost.

### 4. No send names its kernel

Nothing joins a device send to a kernel. Under the host doorbell the time on the
row is when the PROXY rang, which is after the GPU wrote the WQE and may be after
the kernel ended. Not decided yet — see Phase 4.

## Decisions

- **Reader first.** Gaps 1 and 2 are pure SQL over data the corpus already has.
  They ship alone, with a local fixture test, and are checked against the copied
  MN5 tables for case 12.
- **Record `iova`, do not infer it.** A new record type `IbMemRegionIova`
  (`rank, mr, iova`), emitted beside `IbMemRegion` only by the dmabuf door and
  only when `iova != range.base`. Rejected: changing `IbMemRegion` (a struct, and
  the most-read record in the corpus); inferring zero-basedness from
  `raddr < range_size` (true by accident for small VAs on other paths).
  Per the pre-alpha rule, schema churn is allowed, but this one does not need it.
- **Translate in the reader, never in the capture.** The send row keeps the
  `raddr` the NIC saw. `object_access` computes the target VA as
  `mr.range_base + (raddr - iova)` when the peer MR named by `rkey` has an iova
  row, else `raddr`. The NIC's rule exactly: remote VA = MR base + (raddr − iova).
- **Reach the conversion through the export table; share the TABLE, not the
  patcher.** *(Revised while implementing.)* The patcher stays in mlx5dv, which
  every GDAKI run loads; what moved is the fd → range table, into the engine
  (`pcomm_engine_dmabuf_note/_find/_stats`), because the per-provider copies were
  the second half of the bug — the conversion is seen by mlx5dv and needed by
  ibverbs. `providers/mlx5dv/dmabuf_door.hpp` registers
  `cuGetProcAddress_v2`, `cuGetProcAddress` and `cuMemGetHandleForAddressRange`
  with the patcher; the registry gains a dlsym door for `cuGetProcAddress_v2`. Which of the two entries GIN's pointer
  actually comes from is MEASURED on MN5 with `PCOMM_HIJACK_DEBUG=1` before the
  code relies on either.
  - Constraint: the patcher is a process-wide table rewrite. Two providers each
    patching the same entry would see the first's replacement, not the real
    pointer, and silently skip. Hence one patcher (mlx5dv's) and one table.
  - Constraint: patching the export table outside `--enable-doorbell-hijack` is
    a behaviour change. It observes a conversion and returns the real result
    unchanged, but it is still in-memory rewriting of libcuda's table. Gated on
    `PCOMM_DMABUF_EXPORT_TABLE` (default ON whenever mlx5dv is loaded; `0`
    disables) so it can be turned off if a driver disagrees.
- **Kernel attribution is measured before it is designed.**

## Steps

### Phase 1 — devx branch in object_access (reader)

1. Add a `devx` CTE to `object_access`: `mlx5_device_send` → `_peer_of_qp` →
   innermost peer `mem_object` containing the target VA. Same exclusions as
   `_sends` (`decode <= 1`, `opcode != 35`) and the same `raddr > 0` guard as
   `verbs`.
2. `actor_device` = `mlx5_devx_qp.device` for the sending queue, else
   `_rank_device`.
3. Extend `tests/object_access_rule.sh` with devx rows that would be misplaced
   by the classic defects (sender's objects, outermost object, rank device).
4. Check against the copied case 12 tables: expect 1,347 rows (less padding and
   DUMP), all `exact`.
5. Add a case 12 assert: every decoded devx write with `raddr > 0` lands in
   `object_access`.

### Phase 2 — zero-based MRs (record + reader)

1. `IbMemRegionIova` through the full schema path (`.fbs`, generated header,
   conformance, reader table, hash).
2. Emit it from `reg_dmabuf_mr` (ibverbs) and `mlx5dv_reg_dmabuf_mr` if it is
   hooked.
3. Reader: the `devx` and `verbs` branches translate through it by
   `(peer_rank, rkey)`.

### Phase 3 — reach GIN's conversion

1. Probe on MN5: case 26 with `PCOMM_HIJACK_DEBUG=1` and the export-table patcher
   carrying `cuMemGetHandleForAddressRange` + `cuGetProcAddress_v2`. Record
   which entry is patched and whether the fd table fills.
2. Move the patcher to core, have the dmabuf registry register with it.
3. Case 26 assert: every GIN put resolves to the peer's window object.

### Phase 4 — kernel attribution (investigate)

1. Run case 26 and case 12 with kernel timing on (`PCOMM_KERNEL_TIMING`) and the
   host doorbell, and measure how far a doorbell's time lies after the issuing
   kernel's span on the same device.
2. Decide from that: an exact join where exactly one kernel on the queue's
   device can have produced the WQE, `exact = FALSE` otherwise, or no column.

## Follow-ups found on the way

- `mlx5dv_reg_dmabuf_mr` is hooked by nothing. GIN tries it FIRST
  (`MLX5DV_REG_DMABUF_ACCESS_DATA_DIRECT`, CUDA ≥ 12.8, gin_host_gdaki.cc:114)
  and falls back to `ibv_reg_dmabuf_mr` only when it fails. On MN5's NICs it
  fails, which is why case 26's window is recorded at all. On a data-direct
  NIC the window would be registered through a door we do not see, and its
  puts would name an rkey with no region. Needs a hook in the mlx5dv provider
  with the same iova record.
- DC targets: `_peer_of_qp` does not resolve `dest_dct`, so a DCI's sends have
  no peer and no object. Counted nowhere yet.

## Constraints

- `edge_sizes` must still roll up exactly to `comm_edges`; nothing here touches
  `_sends`.
- No synthesized base. An MR whose VA cannot be recovered stays at base 0 and
  produces no `MemObject`, and its sends stay out of `object_access`. `link_health`
  should count them.
- DC targets (`dest_dct`) are not resolved by `_peer_of_qp` today. Out of scope;
  counted, not guessed.

## What actually happened

- **Phase 1.** Re-linked 45925495: case 12's 1,347 devx sends all placed, 640
  puts = 81,920 B per rank in the peer's 4,096 B buffer. MN5 48812895 runs the
  new assert on the cluster's own Parquet: ok.
- **Phase 3, measured.** The export table IS the route: MN5 48812008/48812895
  print `export_table=33` (GIN) and `export_table=20` (NVSHMEM), with
  `registrations resolved=all unresolved=0`. The first version recursed on
  case 12's control (SIGSEGV, one frame thirty deep): the door's own wrapper
  came back from cuGetProcAddress and was adopted as real. Fixed by adopting
  only pointers dladdr places in libcuda (0eed85a).
- **Phase 2, refined.** With the VA recovered, 40 ordinary proxy-plane verbs
  sends were still dropped: NCCL reuses rkeys between 4 KiB ordinary regions
  and zero-based dmabuf ones. An address inside an ordinary region of its key
  is now taken as a VA (_mr_plain, 980f7ee). 48812008 re-linked:
  object_access = comm_edges exactly, unplaced_offset_sends = 0; MN5 48813464
  runs case 26's gin_puts_land_in_the_window: ok (27 checks).
- **Phase 4, measured then built.** MN5 48813620 (case 26 + cupti): all 81,920
  doorbells fell inside exactly one kernel window, always put_ring, and each of
  the 40 put_ring executions held exactly 2,048 sends — its own iteration. So a
  window join on the queue's GPU is exact for kernels that wait on their own
  puts. `kernel_sends` (one row per kernel execution, sends/bytes/peers,
  `shared_sends` for overlaps, never split) and
  `link_health.device_sends_outside_kernels` (a note: fire-and-exit kernels can
  end before a host proxy rings). Fixture: tests/kernel_sends_rule.sh;
  deleting the per-GPU match or the HostSubmit exclusion turns it red.
- **Not yet measured:** NVSHMEM (case 12) with cupti — the same join should hold
  for kernels that quiet, and does not for ones that do not.
