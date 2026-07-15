# NVBit backend (phase-1 default)

`pcomm::gpu::IGpuBackend` implementation over NVBit — the port target is
`../../../snoopie/src/mem_multigpu` (NVBit 1.7.7.1 vendored in
`../../../snoopie/core`), **confirmed working on du04** (4x A100, CUDA 13.0,
driver 580.95; diim test 64/64 with source-line + object attribution,
2026-07-15).

Port plan:

- [ ] Vendor NVBit headers here (EULA: NVBit is redistributable in binary/header form).
- [ ] `NvbitBackend : IGpuBackend` — `instrument()` = today's
      `nvbit_at_function_first_load` instrumentation pass; `make_collector()`
      wraps the device->host channel; `clock_sample()` pairs globaltimer/TSC.
- [ ] Device-side collector fills neutral `attr::RawAccess` (strip vendor types
      from the channel structs).
- [ ] Attributors (Device/Object/SourceLine/CudaIpc) move to the gpumem
      provider — they are vendor-free and shared with the Sanitizer backend.

Build notes for du04: needs `PATH+=/usr/local/cuda/bin:/opt/nvidia/hpc_sdk/Linux_x86_64/25.9/cuda/13.0/bin`
(base CUDA install lacks nvdisasm) and `NVSHMEM_HOME=/home/iturimbetov18/nvshmem-2.10`.
