# Compute-Sanitizer backend (phase 2)

`pcomm::gpu::IGpuBackend` over the Compute-Sanitizer API — the portable,
NVBit-independent path. Port target: `../../../snoopie-sanitized/src/nvidia`
(its `interfaces.cuh` is what `pcomm::attr` was promoted from, so the mapping
is direct). A working reference lives on du04 at `~/sanitizer_demo`
(MemoryTracker + patches).

Stub until the NVBit backend is functional end-to-end.
