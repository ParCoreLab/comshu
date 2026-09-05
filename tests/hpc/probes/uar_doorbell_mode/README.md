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
