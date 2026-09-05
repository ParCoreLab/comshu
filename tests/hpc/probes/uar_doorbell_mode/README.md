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

## Still open: the host path has never captured a doorbell

Arming is proven; **decoding is not**. `mn5_cpu_assisted.sbatch` is the run that
would prove it — two nodes, `NVSHMEM_IBGDA_NIC_HANDLER=cpu`, a real
`shmem_put_bw` — and it is blocked on NVSHMEM bootstrap on MN5, not on anything
in pcomm. Three attempts, each failing further along:

1. `45447084` — `libcudart.so.12` not on `LD_LIBRARY_PATH`; the SDK's perftest
   binaries need the SDK's own CUDA tree. Fixed.
2. `45447108` — `srun --mpi=pmi2` + the SDK's PMI-1 client:
   `mpi/pmi2: unsupported PMI version: 1.0`. SLURM killed the ranks and left
   srun hanging. Switched to `--mpi=pmix` (this SLURM has `pmix_v4`).
3. `45447290`, `45447299` — NVSHMEM reaches `nvshmemi_init_thread` and aborts
   with `non-zero status: 7` (ENOENT). This SDK ships the PMIx bootstrap only as
   `nvshmem_bootstrap_pmix.so.3`; NVSHMEM dlopens the unsuffixed name.
   `NVSHMEM_BOOTSTRAP_PMIX_PLUGIN=nvshmem_bootstrap_pmix.so.3` did not change
   it, so either that is not the variable this version reads or the ENOENT is a
   different file.

Next thing to try, cheapest first: `strace -f -e openat` on one rank to see which
file it actually wants; a symlinked `nvshmem_bootstrap_pmix.so` in a writable dir
ahead on `LD_LIBRARY_PATH`; or sidestep PMIx entirely with
`NVSHMEM_BOOTSTRAP=UID` (the SDK ships `nvshmem_bootstrap_uid.so`, unversioned)
or `NVSHMEM_BOOTSTRAP=MPI` under the site impi. None of this touches pcomm.

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
