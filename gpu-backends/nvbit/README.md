# NVBit backend (phase-1 default)

GPU capture engine over NVBit, feeding `pcomm::gpumem::Engine` directly
through its callbacks (no abstraction layer sits between them — a shared
backend interface gets introduced when a second backend exists to share
it; see `../sanitizer/README.md`). The port target is
`../../../snoopie/src/mem_multigpu` (NVBit 1.7.7.1 vendored in
`../../../snoopie/core`), **confirmed working on du04** (4x A100, CUDA 13.0,
driver 580.95; diim test 64/64 with source-line + object attribution,
2026-07-15).

Status (2026-07-15): **capture path ported and validated on du04** — diim test
reproduces the snoopie baseline (64/64 remote accesses, object attribution,
source-line artifact, clock anchors). Files:

- `nvbit-1.7.7.1/` — vendored NVBit (headers + libnvbit.a + channel utils).
- `nvbit_backend.cu` — nvbit_at_* callbacks; instrumentation pass; recv thread
  translating `pcomm_gpu_access_t` -> `attr::RawAccess` -> `gpumem::Engine`.
- `inject_funcs.cu` / `dev_common.h` — device-side instrumentation function
  (warp-coalesced channel pushes, per-instruction access size).

**Exit-safety invariant** (learned the hard way): NVBit destroys contexts from
an atexit handler that runs *after* C++ static destructors and *after*
main-thread TLS destructors. Everything reachable from nvbit callbacks is
therefore deliberately leaked (engine, Globals, Tracer, intern tables, TLS
buffer stores), and recording threads must call `Engine::drain_thread()`
before exiting — buffers do not auto-drain.

Not yet ported (follow-ups): NVSHMEM heap ownership (2.x range + 3.x cuMemMap
regions), sampling (SAMPLE_SIZE), estimated line carry-forward for instructions
without line info (snoopie's prev-valid fallback + nvdisasm path), python/NCCL
attribution, data-object callstack attribution (adm), cuMemcpyDtoD synthesis,
a vendor-neutral backend interface once the Sanitizer port makes it real.

Build on du04 (no spack needed): `PATH=$HOME/.local/bin:/usr/local/cuda/bin:$PATH`
(pip-installed cmake ≥3.26; node cmake 3.23 lacks CUDA20), `CUDACXX=/usr/local/cuda/bin/nvcc`,
`cmake -B build -DPCOMM_PROVIDER_GPUMEM=ON`. At *run* time NVBit needs nvdisasm:
`PATH+=/opt/nvidia/hpc_sdk/Linux_x86_64/25.9/cuda/13.0/bin`.
