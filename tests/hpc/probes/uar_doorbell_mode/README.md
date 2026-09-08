# uar_doorbell_mode — which doorbell mechanism does this cluster need?

`probe.c` allocates one DEVX UAR the way a GPU-initiated transport does, holds a
CUDA context so the driver is loaded, and exits. It sends nothing. Its only job
is to make the mlx5dv provider's two doorbell paths **state which one this
platform can use**, without needing an NVSHMEM job or a reservation.

```
gcc -O0 -o uar_probe probe.c -I/usr/local/cuda/include \
    -L/usr/local/cuda/lib64/stubs -libverbs -lmlx5 -lcuda

PCOMM_PROVIDER_PATH=<build>/providers/mlx5dv \
  pcomm run -p mlx5dv --enable-host-doorbell -o out -- ./uar_probe
```

## What the answers mean

The host path probes the driver with the exact call NVSHMEM's
`ibgda_nic_mem_gpu_map` makes — `cuMemHostRegister(uar->reg_addr, 8,
Portable|Mapped|IoMemory)` — and reports one of three things:

| It says | The platform | Use |
|---|---|---|
| `DOES map the NIC doorbell page into the GPU` | GPU submission works | `--enable-doorbell-hijack` |
| `driver refuses … NOT_PERMITTED` | no PeerMappingOverride; CPU-assisted only | `--enable-host-doorbell` |
| `not the refusal this guard looks for` / `cannot resolve the CUDA driver` | unproven | neither — it fails closed |

Both platforms measured 2026-09-05, and they answer opposite:

| Host | Answer | `uars_armed` | Reading |
|---|---|---|---|
| **du04** (A100, driver 580) | `DOES map` | 0 | GPU submission works → `--enable-doorbell-hijack` |
| **MN5** `as01r4b07` (H100, driver 595.71.05, `Dual MIT/GPL`) | `NOT_PERMITTED` | **1** | open kernel module, no PeerMappingOverride → host path |

Job `45446991`. This retires the one claim in
`plans/PLAN-20260905-1224-cpu-assisted-ibgda-is-the-cluster-we-have-todo.md`
that was inferred from the experiments repo's env notes rather than measured
here: MN5 cannot do GPU-submitted IBGDA, and the host path arms there and only
there. In the same job the GPU hijack correctly reports it cannot engage
("DEVX allocated 1 UAR(s) and NONE was mapped into the GPU"), so both mechanisms
name the platform correctly from opposite sides.

## The host path captures, on MN5 — job 45449988

Two nodes, `shmem_put_bw`, NVSHMEM's own log confirming
`NIC handler will be CPU with gdrcopy backend`:

```
rank 0: uars_armed=4 doorbells=355 sends=7232 (unknown_op=0 truncated=0) wqes_lapped=0
rank 1: uars_armed=4 doorbells=31  sends=29   (unknown_op=0 truncated=0) wqes_lapped=0
```

7,202 `rdma_write` rows decoded `full` (addresses, keys, lengths) and 59
`atomic_masked_cs` marked `partial` — the remote address read, the local
descriptor left alone behind operand segments this decoder does not model. Zero
unknown opcodes, zero truncated, zero lapped. ~20 WQEs per doorbell, consistent
with NVSHMEM batching the doorbell above the register.

This is the first GPU-initiated data plane pcomm has ever captured on MN5.

### What it took, and the two mistakes worth keeping

**The sinks were installed for one mode.** Job 45449944 armed 4 UARs and decoded
381 doorbells and wrote **nothing** — no stream 212, no 214 — because
`hijack::set_sinks` was still guarded by `hijack::enabled()` alone. The decoder
incremented its counters, found a null sink and returned, so the shutdown line
claimed hundreds of doorbells over an empty capture. Four call sites had been
widened to cover both modes and that one was missed.

**The bootstrap hunt cost four jobs to two confusions.** NVSHMEM has two
variables — `NVSHMEM_BOOTSTRAP` (PMI | MPI | SHMEM | UID) and
`NVSHMEM_BOOTSTRAP_PMI` (PMI | PMI-2 | PMIX) — and `PMI2` is not legal in the
first. It says so plainly ("Missing init flags for bootstrap PMI2") and then
reports errno 7, which prints as *No such file or directory*. That sent this
after imaginary missing plugins twice, including a written-down claim that the
SDK shipped the PMIx bootstrap only as `.so.3`. It does not: the unsuffixed
files are all there, and that claim came from reading a `head -8` truncation as
a fact. Read the log, not the symptom.

## The DC address vector — measured, with the width on the row (job 45462619)

`mn5_dc_av.sbatch`, three legs, one node pair. Since `Mlx5DeviceSend.av_ds`
landed, the capture states the AV width instead of leaving it to be inferred:

| leg | config | rdma_write | `av_ds` | `dest_dct` |
|---|---|---|---|---|
| `rc` | default (2 RC/PE) | 7,202 `full` | **0** (no AV) | `0x000000` |
| `dc_half` | `NUM_RC_PER_PE=0` | 7,202 `full` | **1** (compact, 16 B) | `0x0090f9` |
| `dc_full` | + `IB_FORCE_GRH=1` | 7,202 `full` | **1** — *still compact* | `0x009110` |

**Proven: RC and compact-AV DC.** `dest_dct` is populated and varies across
peers, every rdma_write decodes `full`, and the length histogram is identical to
the RC leg (480 rows each at 4 KiB…128 KiB) — the check that matters, since
`length` sits after the AV and a mis-derived width would shift it.

**Unexecuted at the time: the full AV (`av_ds=3`).** Since job 45580050 it is
not — see the next section, which also records why the conclusion drawn here was
premature. `NVSHMEM_IB_FORCE_GRH=1` does not
flip `support_half_av_seg` in the NVSHMEM this cluster runs. Worth noting that
that build is the CUDA 12.9 SDK one (`r12.9/main_nvshmem` in its own log), not
the 3.7.2 source this repo vendors under `reference/` — so source read there does
not necessarily describe the binary running here.

The difference from job 45452104 is the point of the field. Then, the only way
to notice `dc_full` had not run was archaeology on WQE index strides (`ds=6`
occupies two WQEBBs, `ds=4` one). Now the capture says `av_ds=1` and there is
nothing to infer.

**How the ds=6 branch actually gets proven:** a RoCE port takes it
unconditionally (`lid == 0` ⇒ `support_half_av_seg = false`). That much was
right. What followed it here — "so this needs the paper's P-RoCE platform, B200
+ CX-7 RoCEv2, and until such hardware exists the branch stays unexercised" —
was wrong, and wrong in the cheapest possible way: nobody had run `ls
/sys/class/infiniband`.

## The full AV, on the cluster we already had (job 45580050)

MN5's acc nodes carry **six** mlx5 devices, not the one every case had been
using. Four are the 200 Gb/s NDR InfiniBand HCAs; `mlx5_2` is a 25 Gb/s
**Ethernet** port with RoCE v2 GIDs at index 3 and 5, `lid=0x0`. It is the
service NIC — three PCIe hops from the GPU (`SYS` in `nvidia-smi topo`), on a
management subnet with no reason to have PFC configured — and none of that
matters for exercising a decoder. `mn5_roce_av.sbatch` points `NVSHMEM_HCA_LIST`
at it:

| leg | device | GID | rdma_write | `av_ds` | `dest_dct` |
|---|---|---|---|---|---|
| `roce_rc` | `mlx5_2` | 3 (10.2/16) | 7,203 `full` | **0** (no AV) | `0x000000` |
| `roce_dc` | `mlx5_2` | 3 (10.2/16) | 7,203 `full` | **3** — *the full 48 B AV* | set |
| `roce_dc_vlan` | `mlx5_2` | 5 (VLAN 499) | 7,203 `full` | **3** | set |
| `roce_dc_hostbuf` | `mlx5_2` | 3, `NIC_BUF_MEMTYPE=hostmem` | — | — | 652 doorbells, **0 sends** |

**The ds=6 branch has now executed.** Twice, over two different GIDs on two
different subnets, which is replication rather than a single lucky run. It ran
at 2.87 GB/s — ~23 of the link's 25 Gb/s — so the transport really was on the
Ethernet port and had not silently fallen back to an IB HCA, which is the way a
probe like this produces a false positive.

The evidence that the *decode* is right, not merely that the branch ran: the
rdma_write length histogram is **identical to the IB legs** — 480 rows each at
4 KiB, 8 KiB, 16 KiB, 32 KiB, 64 KiB, 128 KiB. `length` sits after the address
vector, so a width off by 32 bytes shifts it into garbage. Same lengths across
`av_ds` 0, 1 and 3 means all three offset computations agree with the wire.

Two suite assertions now hold this in place, so the branch cannot regress
quietly the way it was silently unexercised: `av_width_is_stable_per_qp` (the
width is re-derived per WQE from bytes at a computed offset, so a drifting
parser shows up as one queue pair reporting two widths) and
`dc_send_names_its_destination` (the DCT and the ext bit come from the same 32
bits, so a plausible width with a zero destination is a read from the wrong
place).

**`roce_dc_hostbuf` is an open loose end, recorded rather than explained.** With
the NIC buffers asked for in host memory the run completed and moved data, the
repeater saw 652 doorbells, and the decode emitted **zero** sends. That is the
doorbell-without-WQE shape, and the likely cause is mundane — `hostmem` is
probably not the token NVSHMEM wants, or the copy-out declines a send queue that
is not GPU-resident, which would be correct behaviour. It is not on the path to
anything the other three legs prove, and it is written down here so it is not
rediscovered as a mystery.

## What it costs (job 45490908) — the forward is free, the copy-out is not

`mn5_overhead.sbatch`: four arms, one allocation, one node pair, three
interleaved repeats, scored on `shmem_put_bw`'s own steady-state bandwidth
(GB/s, higher is better; the 24.3 plateau is ~97% of MN5's 200 Gb/s link).

| bytes | bare | passive | forward | full | passive/bare | forward/bare | **full/bare** |
|---|---|---|---|---|---|---|---|
| 1 KiB | 0.10 | 0.10 | 0.09 | 0.08 | 1.004 | 0.958 | **0.880** |
| 8 KiB | 0.71 | 0.71 | 0.72 | 0.62 | 1.001 | 1.019 | **0.883** |
| 16 KiB | 1.40 | 1.41 | 1.43 | 1.21 | 1.006 | 1.025 | **0.867** |
| 128 KiB | 11.08 | 10.76 | 11.05 | 9.93 | 0.972 | 0.997 | **0.897** |
| 512 KiB | 21.52 | 21.57 | 21.58 | 20.87 | 1.002 | 1.003 | **0.970** |
| 4 MiB | 24.28 | 24.29 | 24.28 | 24.13 | 1.000 | 1.000 | **0.994** |

Sizes ≤256 B are omitted: their bandwidth rounds to 0.00–0.03 GB/s and the
ratios there are noise, not measurement.

Each arm engaged as intended, which the doorbell counters confirm rather than
the ratios: `forward` recorded 444 doorbells and **0 sends**
(`PCOMM_HIJACK_NO_DECODE` doing its job), `full` recorded 391 doorbells and
7,232 sends, `passive` and `bare` neither.

**The decomposition.** The ordinary control-plane interposers cost nothing
measurable (passive/bare within ±3%, i.e. inside run-to-run noise). The decoy
page and the repeater's forward *also* cost nothing measurable (forward/passive
within the same band) — while carrying every doorbell the application rang. The
entire cost is the per-doorbell `cuMemcpyDtoHAsync` + `cuStreamSynchronize`
copy-out: up to **13% of bandwidth at 16 KiB**, fading to under 1% by 4 MiB as
the wire saturates and a fixed per-doorbell cost amortises.

**This inverts the premise the on-GPU repeater was planned on.**
`PLAN-20260807-2344` justifies Stage 3 as removing "a host round trip per
doorbell" — the forward. On the host path the forward is free and the copy-out
is everything, so an on-GPU repeater would not move this number. The plan is not
wrong: it was written about the GPU hijack on du04, where the forward really is
GPU → host → BAR. But for the mode our largest cluster actually runs, the
optimisation target is the copy-out, and the cheap fixes are not a persistent
kernel — stop synchronising per doorbell, or read the send queue through a
host mapping (GDRCopy is present on MN5) instead of a CUDA copy at all.

This is also the first use of `PCOMM_HIJACK_NO_DECODE` for the question it was
added to answer. Its own comment says the published "+68% forward / +86%
forward+decode" could not separate the two because they were wall-clock over a
startup-dominated run. Scored on the application's steady-state numbers, they
separate cleanly, and the answer is not the one those figures implied.

## ...and on du04, where the forward is the whole cost instead (3 reps)

`a100_overhead.sh`, the same four arms and benchmark on the GPU hijack path.
Loopback (both PEs behind one HCA), so absolute GB/s is not comparable to MN5's
— the ratios are the point. Arms engaged: `forward` 298 doorbells / **0 sends**,
`full` 104 / 7,153.

| bytes | passive/bare | **forward/bare** | full/bare |
|---|---|---|---|
| 512 | 1.011 | **0.023** | 0.022 |
| 4 KiB | 0.987 | **0.032** | 0.020 |
| 32 KiB | 1.001 | **0.035** | 0.034 |
| 128 KiB | 0.988 | **0.062** | 0.060 |
| 1 MiB | 1.001 | **0.384** | 0.331 |
| 4 MiB | 1.000 | **0.540** | 0.518 |

**The two doorbell paths cost opposite things, and neither is what the other
would predict:**

| | forward | decode on top | dominant cost |
|---|---|---|---|
| host path (MN5, CPU-assisted) | free (±3%) | up to 13% | the per-doorbell copy-out |
| GPU path (du04, GPU-submitted) | **16–45× loss** | negligible beside it | the GPU → host → BAR round trip |

On the GPU path the forward costs 95–98% of throughput from 256 B to 128 KiB,
and the decode is noise on top of a path already destroyed by the round trip.
On the host path the forward is a host→host store and costs nothing, so the
copy-out is all that is left to pay for.

**So PLAN-20260807-2344 is strongly justified — for exactly one of the two
paths, the one it was written about.** An on-GPU repeater removes the forward:
on du04 that is the entire cost and worth a 20–40× recovery; on MN5 it would
change nothing measurable. The earlier annotation on that plan scoped this
correctly but had only half the evidence; both halves are now measured.

It also shows the published "+68% forward, +86% forward+decode" understated the
steady-state cost by more than an order of magnitude, exactly as
`uar_hijack.hpp` warned: those were wall-clock over a startup-dominated run.

**One thing the counters give away that the ratios do not.** The `full` arm rang
104 doorbells where `forward` rang 298, for the same workload and nearly the
same send count (7,153 vs 7,232). A slower repeater means the GPU has more
ready-but-unposted WQEs when it next checks, so it batches more per doorbell.
The instrument therefore changes the *batching* it is measuring, not only the
timing — which is worth remembering before reading WQEs-per-doorbell off a
hijacked capture as a property of the application.

## Superseded: the bootstrap dead end, and what was wrong about it

Kept because the wrong turns are the useful part. Four jobs before 45449988:

1. `45447084` — `libcudart.so.12` not on `LD_LIBRARY_PATH`; the SDK's perftest
   binaries need the SDK's own CUDA tree. Real, fixed.
2. `45447108` — `srun --mpi=pmi2` against the SDK client:
   `mpi/pmi2: unsupported PMI version: 1.0`. SLURM killed the ranks and left
   srun hanging. Real.
3. `45447290`, `45447299`, `45449905` — NVSHMEM aborts in `nvshmemi_init_thread`
   with `non-zero status: 7`.

> **The diagnosis written here for (3) was WRONG.** It said this SDK ships the
> PMIx bootstrap only as `nvshmem_bootstrap_pmix.so.3` while NVSHMEM dlopens the
> unsuffixed name. It does not — `nvshmem_bootstrap_{pmi,pmi2,pmix,mpi,uid}.so`
> are all present, unsuffixed. That claim came from reading a `head -8`
> truncation of the directory listing as a complete one, and it survived into
> this file as fact for one commit.

The actual cause, from NVSHMEM's own `NVSHMEM_DEBUG=INFO` log, which was on the
whole time and being filtered out by an over-aggressive grep:

```
bootstrap.cpp:bootstrap_flag2mode:58: Missing init flags for bootstrap PMI2.
                                      Retry with nvshmemx_init_attr and non-zero flags
```

`NVSHMEM_BOOTSTRAP` takes the bootstrap FAMILY (`PMI` | `MPI` | `SHMEM` | `UID`)
and `NVSHMEM_BOOTSTRAP_PMI` takes the PMI VARIANT (`PMI` | `PMI-2` | `PMIX`).
`PMI2` is not legal in the first. NVSHMEM then reports errno 7, which prints as
*No such file or directory* and reads exactly like a missing plugin — which is
what sent this after imaginary files, twice.

`NVSHMEM_BOOTSTRAP=PMI` + `NVSHMEM_BOOTSTRAP_PMI=PMI-2` + `srun --mpi=pmi2` is
what works, and matches the paper's own MN5 harness
(Dissecting-IBGDA-Experiments, branch `mechanism-v3`, `run_e7_mn5.sh`).

## Why the probe is worth keeping

The guard was wrong twice before it was right, and only a real run showed it:

1. A 4096-byte flagless probe returned `CUDA_ERROR_INVALID_VALUE` on du04 — a
   platform that *does* support GPU submission — and the guard read that as a
   refusal and armed. Arming there would have handed the transport ordinary host
   memory where it expected I/O memory and changed which submission mode it
   picked, which is an instrument altering its subject.
2. Fixing the flags but not the size left it still wrong, because NVSHMEM
   registers **8 bytes at the unaligned `reg_addr`**, not a page at its base.

Both were found by running this, not by reading. That is why it is a checked-in
probe and not a paragraph.
