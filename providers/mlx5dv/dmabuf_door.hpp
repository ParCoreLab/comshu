#pragma once
// The dmabuf conversion, reached through the CUDA driver export table.
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
// So this registers three replacements with the export-table patcher —
// cuGetProcAddress_v2, cuGetProcAddress, and the conversion itself — whichever
// of them cudart's table actually carries. Each one calls the real driver
// function and returns its result untouched; the only thing it changes is that
// a lookup of the conversion hands back the observing wrapper, which in turn
// returns exactly what the driver returned. Which door delivered is tallied in
// the engine (door 3) and printed at shutdown, so a run states it.
//
// ARMED BY DEFAULT, unlike the doorbell hijack, because it is not in any data
// path: it sits on a once-per-registration conversion, observes it, and passes
// through. It IS a rewrite of libcuda's export table in memory, so
// PCOMM_DMABUF_EXPORT_TABLE=0 turns it off if a driver ever disagrees.

#include <cstdlib>

#include "cuda_export_table.hpp"
#include "pcomm/core/dmabuf_registry.hpp"

namespace mlx5::dmabuf_door {

namespace dmb = pcomm::core::dmabuf;

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_DMABUF_EXPORT_TABLE");
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
        dmb::substitute_if_conversion<dmb::kDoorExportTable>(symbol, pfn);
    return rc;
}

inline dmb::CuResult get_proc_address(const char* symbol, void** pfn, int driver_version,
                                      unsigned long long flags) {
    auto* real = reinterpret_cast<dmb::PfnGetProcAddress>(export_table::real("cuGetProcAddress"));
    if (!real)
        return 1 /* CUDA_ERROR_INVALID_VALUE */;
    const dmb::CuResult rc = real(symbol, pfn, driver_version, flags);
    if (rc == 0)
        dmb::substitute_if_conversion<dmb::kDoorExportTable>(symbol, pfn);
    return rc;
}

inline dmb::CuResult get_handle(void* handle, dmb::CuDevicePtr dptr, std::size_t size,
                                int handle_type, unsigned long long flags) {
    if (!dmb::real_get_handle())
        dmb::real_get_handle() = reinterpret_cast<dmb::PfnMemGetHandle>(
            export_table::real("cuMemGetHandleForAddressRange"));
    return dmb::get_handle_for_address_range<dmb::kDoorExportTable>(handle, dptr, size,
                                                                     handle_type, flags);
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
    pcomm_engine_dmabuf_stats(door, &hits, &misses);
    if (!(door[0] | door[1] | door[2] | door[3] | hits | misses))
        return;
    std::fprintf(stderr,
                 "[pcomm rank %d] dmabuf: conversions dlsym=%llu getproc=%llu getproc_v2=%llu "
                 "export_table=%llu | registrations resolved=%llu unresolved=%llu%s\n",
                 rank, (unsigned long long)door[0], (unsigned long long)door[1],
                 (unsigned long long)door[2], (unsigned long long)door[3],
                 (unsigned long long)hits, (unsigned long long)misses,
                 enabled() ? "" : " (export-table door off: PCOMM_DMABUF_EXPORT_TABLE=0)");
}

}  // namespace mlx5::dmabuf_door
