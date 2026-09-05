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

Measured 2026-09-05 on **du04**: `DOES map`, so du04 is the GPU-hijack platform
and the host path correctly refuses to arm there. **MN5 is expected to answer
`NOT_PERMITTED`** — its open kernel module has no `PeerMappingOverride`, which is
what forces `NVSHMEM_IBGDA_NIC_HANDLER=cpu` there and is the whole reason the
host path exists. **That has not been run yet**; it is the first thing to do with
an MN5 allocation, because it is the one claim in
`plans/PLAN-20260905-1224-cpu-assisted-ibgda-is-the-cluster-we-have-todo.md`
that is inferred from someone else's run notes rather than measured by us.

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
