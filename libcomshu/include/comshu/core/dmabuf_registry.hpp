#pragma once
// fd -> GPU address range, for buffers the NIC is given by dmabuf descriptor.
//
// Under DMABUF registration a umem carries `addr = 0x0` and a file descriptor,
// so `sq_base` is an OFFSET and the buffer has no address anywhere in the
// capture. That is why mlx5dv emits no MemObject for a by_dmabuf umem and why
// `gpu_driven` falls back to "the binding was dmabuf" instead of asking CUDA —
// a heuristic that made case 09's engagement gate circular.
//
// The address is not unrecoverable, it is simply upstream. NVSHMEM had a device
// pointer and called cuMemGetHandleForAddressRange(&fd, dptr, size,
// CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0) to convert it. Catching that call
// yields fd -> (dptr, size) directly, and it happens BEFORE the registration, so
// nothing has to be back-filled into an append-only stream.
//
// Two doors, because LOAD_SYM picks between them at runtime
// (nvshmem-3.7.2 src/host/init/cudawrap.cpp:38-45):
//
//   primary   pfn_cuGetProcAddress("cuMemGetHandleForAddressRange", &pfn, ...)
//   fallback  dlsym(cudaLib, "cuMemGetHandleForAddressRange")
//
// Both are registered. Covering only the primary would work today and break on
// a build where cuGetProcAddress fails to resolve, which is precisely the kind
// of version-luck this file exists to avoid.
//
// No CUDA headers: this must build where there is no CUDA at all (MN5's core
// configuration does). The types below are hand-copied from cuda.h and NOTHING
// CHECKS THEM — the same exposure that left kAttrDeviceOrdinal wrong for the
// mlx5dv provider's entire life. They are pinned by the typedefs NVSHMEM itself
// uses (src/include/internal/host_transport/cudawrap.h:41,173).
//
// THE TABLE IS THE ENGINE'S; THE DOORS ARE HERE. Both ibverbs and mlx5dv need
// `fd -> address`: mlx5dv for DEVX umems, ibverbs for ibv_reg_dmabuf_mr. Each
// provider calls install() and interposes the same CUDA calls itself — any
// provider may hook CUDA, because a provider may need allocation facts for its
// own runtime decisions and cannot wait on another provider for them — but what
// either one learns goes into ONE table in the engine (comshu_engine_dmabuf_*).
//
// It used to be one table per provider, by copy, and that was the defect that
// left NCCL GIN's window unplaced (MN5 job 45925495, case 26: 81,920 puts into a
// zero-based region with no VA). GIN's conversion is reachable only through the
// CUDA driver export table, which only mlx5dv patches; its region is registered
// through ibv_reg_dmabuf_mr, which only ibverbs records. The fact was learned by
// one provider and needed by the other.
//
// FOUR DOORS, because callers reach the conversion four ways:
//
//   0  dlsym(libcuda, "cuMemGetHandleForAddressRange")   NVSHMEM's fallback
//   1  cuGetProcAddress(...)                             NVSHMEM's primary
//   2  cuGetProcAddress_v2(...)                          the CUDA 12 entry point
//   3  the driver export table, patched by mlx5dv        cudart_static's route:
//      NCCL resolves it with cudaGetDriverEntryPointByVersion from a STATICALLY
//      linked cudart (nccl src/misc/cudawrap.cc), which reaches the driver
//      through cuGetExportTable and never through a dynamic symbol.
//
// The door a conversion arrived by is tallied in the engine, so a run says which
// one actually delivered rather than which ones were armed.

#include <dlfcn.h>

#include <cstdint>
#include <cstring>

#include "comshu/core/mem_attr.hpp"
#include "comshu/engine_abi.h"
#include "comshu/core/vocab.hpp"
#include "comshu/hook/dlsym/Dlsym.hpp"

namespace comshu::core::dmabuf {

// cuda.h: CUresult is an enum (int); CUdeviceptr is unsigned 64-bit on LP64.
using CuResult = int;
using CuDevicePtr = unsigned long long;

// CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD. The only handle type that yields an fd
// the NIC can import; anything else is not the conversion we are looking for.
inline constexpr int kDmaBufFd = 0x1;

using PfnMemGetHandle = CuResult (*)(void*, CuDevicePtr, std::size_t, int, unsigned long long);
using PfnGetProcAddress = CuResult (*)(const char*, void**, int, unsigned long long);

struct Range {
    std::uint64_t base{0};
    std::uint64_t size{0};
    DeviceId      device{kUnknownDevice};
};

enum Door : std::uint8_t { kDoorDlsym = 0, kDoorGetProc = 1, kDoorGetProcV2 = 2, kDoorExportTable = 3 };

inline void remember(int fd, const Range& r, Door door) {
    comshu_engine_dmabuf_note(fd, r.base, r.size, r.device, door);
}

// A descriptor is only unique while OPEN — closed fds are reused, exactly as
// addresses are. A stale hit is therefore possible in principle; it has not been
// observed, and the durable fix is to key identity on the dmabuf inode (fstat)
// rather than on the number. Recorded rather than silently assumed away.
inline bool lookup(int fd, Range* out) {
    std::uint64_t base = 0, size = 0;
    std::uint16_t device = kUnknownDevice;
    if (!comshu_engine_dmabuf_find(fd, &base, &size, &device))
        return false;
    *out = Range{base, size, static_cast<DeviceId>(device)};
    return true;
}

inline PfnMemGetHandle& real_get_handle() {
    static PfnMemGetHandle fn = nullptr;
    return fn;
}
inline PfnGetProcAddress& real_get_proc_address() {
    static PfnGetProcAddress fn = nullptr;
    return fn;
}

// The interposed conversion, one instantiation per door so the tally can say
// which route the caller took. Records only on success and only for the dmabuf
// handle type, and never changes what the caller receives — this is an observer,
// and the one rule here is that a hook which meets something it does not
// recognise records it and gets out of the way.
template<Door D>
inline CuResult get_handle_for_address_range(void* handle, CuDevicePtr dptr, std::size_t size,
                                             int handle_type, unsigned long long flags) {
    auto* real = real_get_handle();
    if (!real)  // cannot happen via any door; refuse rather than crash
        return 1 /* CUDA_ERROR_INVALID_VALUE */;

    const CuResult rc = real(handle, dptr, size, handle_type, flags);
    if (rc == 0 && handle && handle_type == kDmaBufFd) {
        const int fd = *static_cast<int*>(handle);
        remember(fd,
                 Range{static_cast<std::uint64_t>(dptr), static_cast<std::uint64_t>(size),
                       device_of(reinterpret_cast<const void*>(dptr))},
                 D);
    }
    return rc;
}

// Is `fn` the driver's own code? Only libcuda's function may become `real`.
//
// MEASURED, not anticipated: MN5 job 48812008, case 12's control run died with
// SIGSEGV on a stack of one frame repeated thirty deep inside
// libmlx5dv_trace.so. The export-table door patches the conversion's table
// entry with its own wrapper; a later cuGetProcAddress then handed that wrapper
// back as "the" cuMemGetHandleForAddressRange, it was adopted as real, and the
// wrapper called itself. Excluding our four registry instantiations by address
// (the first guard) could not see it, because the door's wrapper is a fifth,
// and every comshu .so carries its own copies of all of them besides.
//
// So the test is positive rather than an exclusion list: the pointer must come
// from libcuda. Anything else is some interposer — ours in another provider, or
// a third party's — and is left exactly where it is.
inline bool is_driver_fn(const void* fn) {
    Dl_info info{};
    if (!fn || !dladdr(fn, &info) || !info.dli_fname)
        return false;
    return std::strstr(info.dli_fname, "libcuda") != nullptr;
}

// Substitute our wrapper when a lookup hands back the conversion — but only
// when what it handed back is the DRIVER's. If it is another comshu wrapper
// (another provider's door, which records into the same engine table) it is
// already observing, and wrapping it again would at best double-count and at
// worst, as above, recurse. Shared by every entry-point door.
template<Door D>
inline void substitute_if_conversion(const char* symbol, void** pfn) {
    if (!symbol || !pfn || !*pfn || std::strcmp(symbol, "cuMemGetHandleForAddressRange") != 0)
        return;
    if (!is_driver_fn(*pfn))
        return;
    real_get_handle() = reinterpret_cast<PfnMemGetHandle>(*pfn);
    *pfn = reinterpret_cast<void*>(&get_handle_for_address_range<D>);
}

// The primary door. Substitutes the pointer CUDA hands back for the one symbol
// we care about and passes everything else through untouched.
inline CuResult get_proc_address(const char* symbol, void** pfn, int driver_version,
                                 unsigned long long flags) {
    auto* real = real_get_proc_address();
    if (!real)
        return 1 /* CUDA_ERROR_INVALID_VALUE */;

    const CuResult rc = real(symbol, pfn, driver_version, flags);
    // NO comshu_engine_note_cuda_live() HERE, and the absence is a measurement.
    //
    // This door looked like the principled place to report CUDA liveness: it is
    // in core, entering it proves the driver answers, and a statically linked
    // cudart — nvcc's default — reaches the driver through cuGetProcAddress,
    // which is the very reason the memcpy work chose CUPTI over interposition.
    // The inference was wrong, and it was measured TWICE with the call wired in
    // and COMSHU_TRACE_HOOKS=1 on du04, in both contexts where the door can be
    // armed:
    //
    //   copybench, -p cupti, door armed from Tracer::start   0 entries
    //   nccl_collectives, -p ib, door armed by ibverbs       0 entries
    //
    // Both captures were real and non-empty; the second carries ib_device,
    // invocation, capture_status and NCCL's own 912/914 rows. cupti_buffer_request
    // and nccl_comm_init each fired first in their own capture and succeeded.
    // Resolution is not invocation — the trap COMSHU_TRACE_HOOKS exists for, and
    // it caught this proposal instead of confirming it.
    if (rc == 0)
        substitute_if_conversion<kDoorGetProc>(symbol, pfn);
    return rc;
}

// cuGetProcAddress_v2 (CUDA 12+): the same lookup with a status out-param. The
// door cudart_static takes for cudaGetDriverEntryPoint[ByVersion].
using PfnGetProcAddressV2 = CuResult (*)(const char*, void**, int, unsigned long long, int*);
inline PfnGetProcAddressV2& real_get_proc_address_v2() {
    static PfnGetProcAddressV2 fn = nullptr;
    return fn;
}
template<Door D>
inline CuResult get_proc_address_v2(const char* symbol, void** pfn, int driver_version,
                                    unsigned long long flags, int* status) {
    auto* real = real_get_proc_address_v2();
    if (!real)
        return 1 /* CUDA_ERROR_INVALID_VALUE */;
    const CuResult rc = real(symbol, pfn, driver_version, flags, status);
    if (rc == 0)
        substitute_if_conversion<D>(symbol, pfn);
    return rc;
}

// Idempotent: registering twice would wrap our own wrapper, which still works
// but makes the chain harder to read than it needs to be.
inline void install() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    comshu::smart_dlsym::register_symbol(
        "cuGetProcAddress", [](void* next, void*, const char*) -> void* {
            real_get_proc_address() = reinterpret_cast<PfnGetProcAddress>(next);
            return reinterpret_cast<void*>(&get_proc_address);
        });

    comshu::smart_dlsym::register_symbol(
        "cuGetProcAddress_v2", [](void* next, void*, const char*) -> void* {
            real_get_proc_address_v2() = reinterpret_cast<PfnGetProcAddressV2>(next);
            return reinterpret_cast<void*>(&get_proc_address_v2<kDoorGetProcV2>);
        });

    comshu::smart_dlsym::register_symbol(
        "cuMemGetHandleForAddressRange", [](void* next, void*, const char*) -> void* {
            real_get_handle() = reinterpret_cast<PfnMemGetHandle>(next);
            return reinterpret_cast<void*>(&get_handle_for_address_range<kDoorDlsym>);
        });
}

}  // namespace comshu::core::dmabuf
