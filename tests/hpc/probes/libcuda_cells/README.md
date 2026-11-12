# libcuda_cells — where libcuda keeps the pointers it hands out

Evidence for `providers/mlx5dv/cuda_export_table.hpp`.

    nvcc -O1 -o segscan segscan.cu -ldl && ./segscan
    nvcc -O1 -o patch   patch.cu   -ldl && ./patch

`segscan` counts, in libcuda's non-executable PT_LOAD segments, the words equal
to a few driver entry points' addresses — right after `dlopen`, after `cuInit`,
and after (static) cudart init. `patch` overwrites those cells right after
`dlopen` and asks every route for `cuMemGetHandleForAddressRange`.

## du04, 2026-11-12 (A100, HPC SDK 25.9, CUDA 13.0, static cudart)

    [after dlopen] ... non-exec segs=3 bytes=80498304
      cuMemGetHandleForAddressRange  hits=1   (writable segment, inside RELRO)
      cuGetProcAddress_v2            hits=1
      cuMemHostRegister_v2           hits=1
      cuMemHostGetDevicePointer_v2   hits=1
      cuMemAlloc_v2                  hits=1
    [after cuInit]       identical cells, identical counts
    [after cudart init]  identical cells, identical counts

    patched cell 0x7e6ee07c3030 (mprotect rc=0)
    cudart entry point = <ours>     (cudaGetDriverEntryPointByVersion — NCCL's route)
    cuGetProcAddress_v2 = <ours>
    call rc=0 fd=52 calls_through_mine=1

One cell per entry point, fixed from load time, read by both lookup routes.
Overwriting it before the first lookup reaches every asker regardless of how
cudart or NCCL is linked. The scan only needs the writable segment (a pointer
needing relocation lives there), so it is bounded without a sentinel.
