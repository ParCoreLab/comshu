# Compute-Sanitizer backend (phase 2)

GPU capture engine over the Compute-Sanitizer API — the portable,
NVBit-independent path. This port is what introduces the vendor-neutral
backend interface: a sketch existed as `libpcomm/include/pcomm/gpu/backend.hpp`
(IGpuBackend, removed unimplemented — resurrect from git history) and becomes
real here, when there are two backends to sit behind it.
Port target: `../../../snoopie-sanitized/src/nvidia`
(its `interfaces.cuh` is what `pcomm::attr` was promoted from, so the mapping
is direct). A working reference lives on du04 at `~/sanitizer_demo`
(MemoryTracker + patches).

Stub until the NVBit backend is functional end-to-end.
