#pragma once
// The dmabuf conversion, reached through libcuda's own pointer cell.
//
// core/dmabuf_registry.hpp watches cuMemGetHandleForAddressRange through every
// DYNAMIC route: dlsym of the symbol, and cuGetProcAddress[_v2] by dlsym. NCCL
// takes none of them. It resolves its driver entry points with
// cudaGetDriverEntryPointByVersion from a STATICALLY linked cudart
// (reference/nccl-git src/misc/cudawrap.cc:172-199), and cudart_static reaches
// the driver through cuGetExportTable — the same route that made the GPU
// doorbell hijack need export_table.hpp in the first place (measured on du04 for
// NVSHMEM's cudaHostGetDevicePointer: both dynamic doors counted 0 calls).
//
// What that cost: NCCL GIN registers its GDAKI window zero-based (iova 0)
// through a dmabuf whose fd it gets from that conversion, so with the fd
// unresolved the window has no VA and none of its puts can be placed. MN5 job
// 45925495, case 26: 81,920 puts, all unplaced.
//
// The first fix patched cuGetExportTable's table when OUR interposer was asked
// for it; on du04 (NCCL 2.28.9) that was the last of 15 fetches, after NCCL had
// already copied the real pointer: dlsym=168 export_table=0, 24 registrations
// unresolved. cuda_export_table.hpp now overwrites libcuda's one cell per entry
// point before any pointer-getter is handed out, which reaches every asker —
// shared cudart, a cudart static inside libnccl, or a statically linked NCCL.
//
// So this registers three replacements with that patcher —
// cuGetProcAddress_v2, cuGetProcAddress, and the conversion itself. Each one calls the real driver
// function and returns its result untouched; the only thing it changes is that
// a lookup of the conversion hands back the observing wrapper, which in turn
// returns exactly what the driver returned. Which door delivered is tallied in
// the engine (door 3) and printed at shutdown, so a run states it.
//
// ARMED BY DEFAULT, unlike the doorbell hijack, because it is not in any data
// path: it sits on a once-per-registration conversion, observes it, and passes
// through. It IS a rewrite of libcuda's memory, so
// COMSHU_DMABUF_LIBCUDA_CELL=0 turns it off if a driver ever disagrees.

#include <cstdlib>

#include "cuda_export_table.hpp"
#include "comshu/core/dmabuf_registry.hpp"

namespace mlx5::dmabuf_door {

namespace dmb = comshu::core::dmabuf;

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_DMABUF_LIBCUDA_CELL");
        return !(v && *v == '0');
    }();
    return on;
}

inline dmb::CuResult get_proc_address_v2(const char* symbol, void** pfn, int driver_version,
                                         unsigned long long flags, int* status) {
    auto* real = reinterpret_cast<dmb::PfnGetProcAddressV2>(export_table::real("cuGetProcAddress_v2"));
    if (!real)
        return 1 /* CUDA_ERROR_INVALID_VALUE */;
    const dmb::CuResult rc = real(symbol, pfn, driver_version, flags, status);
    if (rc == 0)
        dmb::substitute_if_conversion<dmb::kDoorLibcudaCell>(symbol, pfn);
    return rc;
}

inline dmb::CuResult get_proc_address(const char* symbol, void** pfn, int driver_version,
                                      unsigned long long flags) {
    auto* real = reinterpret_cast<dmb::PfnGetProcAddress>(export_table::real("cuGetProcAddress"));
    if (!real)
        return 1 /* CUDA_ERROR_INVALID_VALUE */;
    const dmb::CuResult rc = real(symbol, pfn, driver_version, flags);
    if (rc == 0)
        dmb::substitute_if_conversion<dmb::kDoorLibcudaCell>(symbol, pfn);
    return rc;
}

// The table entry's replacement. It calls the real the PATCHER captured — the
// pointer that was in libcuda's table before we overwrote it — and never the
// registry's shared real_get_handle(), which another door may have set. Taking
// that one is how this wrapper came to call itself (MN5 job 48812008).
inline dmb::CuResult get_handle(void* handle, dmb::CuDevicePtr dptr, std::size_t size,
                                int handle_type, unsigned long long flags) {
    auto* real = reinterpret_cast<dmb::PfnMemGetHandle>(
        export_table::real("cuMemGetHandleForAddressRange"));
    if (!real || !dmb::is_driver_fn(reinterpret_cast<const void*>(real)))
        return 1 /* CUDA_ERROR_INVALID_VALUE */;
    const dmb::CuResult rc = real(handle, dptr, size, handle_type, flags);
    if (rc == 0 && handle && handle_type == dmb::kDmaBufFd) {
        const int fd = *static_cast<int*>(handle);
        dmb::remember(fd,
                      dmb::Range{static_cast<std::uint64_t>(dptr), static_cast<std::uint64_t>(size),
                                 comshu::core::device_of(reinterpret_cast<const void*>(dptr))},
                      dmb::kDoorLibcudaCell);
    }
    return rc;
}

// Call once from the provider constructor. Idempotent through the patcher.
inline void install() {
    if (!enabled())
        return;
    export_table::register_overwrite("cuGetProcAddress_v2",
                                     reinterpret_cast<void*>(&get_proc_address_v2));
    export_table::register_overwrite("cuGetProcAddress",
                                     reinterpret_cast<void*>(&get_proc_address));
    export_table::register_overwrite("cuMemGetHandleForAddressRange",
                                     reinterpret_cast<void*>(&get_handle));
    export_table::install();
}

// One line, only when the run registered anything by dmabuf: which door the
// conversions came through, and whether registrations found them.
inline void report(int rank) {
    std::uint64_t door[4] = {0, 0, 0, 0}, hits = 0, misses = 0;
    comshu_engine_dmabuf_stats(door, &hits, &misses);
    if (!(door[0] | door[1] | door[2] | door[3] | hits | misses))
        return;
    std::fprintf(stderr,
                 "[comshu rank %d] dmabuf: conversions dlsym=%llu getproc=%llu getproc_v2=%llu "
                 "libcuda_cell=%llu | registrations resolved=%llu unresolved=%llu%s\n",
                 rank, (unsigned long long)door[0], (unsigned long long)door[1],
                 (unsigned long long)door[2], (unsigned long long)door[3],
                 (unsigned long long)hits, (unsigned long long)misses,
                 enabled() ? "" : " (libcuda-cell door off: COMSHU_DMABUF_LIBCUDA_CELL=0)");
}

}  // namespace mlx5::dmabuf_door
