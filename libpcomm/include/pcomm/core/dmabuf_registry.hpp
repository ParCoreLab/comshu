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
// IN CORE, NOT IN A PROVIDER, and shared by copy rather than by reference. Both
// ibverbs and mlx5dv need `fd -> address`: mlx5dv for DEVX umems, ibverbs for
// ibv_reg_dmabuf_mr. Each provider calls install() and gets its OWN table,
// populated by its OWN interception of the same CUDA calls.
//
// That is not a workaround for the lack of shared state, it is the stated design
// rule: any provider may hook CUDA itself, because a provider may need
// allocation facts for its own runtime decisions and cannot wait on another
// provider for them. It only became possible when the dlsym registry learned to
// chain — before that, the second provider to register "cuGetProcAddress" threw
// and aborted the process.
//
// The duplication is real and bounded: two wrappers in the chain, two small
// tables, one extra indirection per CUDA export call. If a single shared table
// is ever wanted instead, it belongs in the engine — a static library linked
// into two .so's gives two copies, which is exactly what this design leans on.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "pcomm/core/mem_attr.hpp"
#include "pcomm/core/vocab.hpp"
#include "pcomm/hook/dlsym/Dlsym.hpp"

namespace pcomm::core::dmabuf {

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

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}
inline std::unordered_map<int, Range>& table() {
    static std::unordered_map<int, Range> t;
    return t;
}

inline void remember(int fd, const Range& r) {
    std::lock_guard<std::mutex> lk(mutex());
    table()[fd] = r;
}

// A descriptor is only unique while OPEN — closed fds are reused, exactly as
// addresses are. A stale hit is therefore possible in principle; it has not been
// observed, and the durable fix is to key identity on the dmabuf inode (fstat)
// rather than on the number. Recorded rather than silently assumed away.
inline bool lookup(int fd, Range* out) {
    std::lock_guard<std::mutex> lk(mutex());
    auto                        it = table().find(fd);
    if (it == table().end())
        return false;
    *out = it->second;
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

// The interposed conversion. Records only on success and only for the dmabuf
// handle type, and never changes what the caller receives — this is an observer,
// and the one rule in this provider is that a hook which meets something it does
// not recognise records it and gets out of the way.
inline CuResult get_handle_for_address_range(void* handle, CuDevicePtr dptr, std::size_t size,
                                             int handle_type, unsigned long long flags) {
    auto* real = real_get_handle();
    if (!real)  // cannot happen via either door; refuse rather than crash
        return 1 /* CUDA_ERROR_INVALID_VALUE */;

    const CuResult rc = real(handle, dptr, size, handle_type, flags);
    if (rc == 0 && handle && handle_type == kDmaBufFd) {
        const int fd = *static_cast<int*>(handle);
        remember(fd, Range{static_cast<std::uint64_t>(dptr), static_cast<std::uint64_t>(size),
                           device_of(reinterpret_cast<const void*>(dptr))});
    }
    return rc;
}

// The primary door. Substitutes the pointer CUDA hands back for the one symbol
// we care about and passes everything else through untouched.
inline CuResult get_proc_address(const char* symbol, void** pfn, int driver_version,
                                 unsigned long long flags) {
    auto* real = real_get_proc_address();
    if (!real)
        return 1 /* CUDA_ERROR_INVALID_VALUE */;

    const CuResult rc = real(symbol, pfn, driver_version, flags);
    // NO pcomm_engine_note_cuda_live() HERE, and the absence is a measurement.
    //
    // This door looked like the principled place to report CUDA liveness: it is
    // in core, entering it proves the driver answers, and a statically linked
    // cudart — nvcc's default — reaches the driver through cuGetProcAddress,
    // which is the very reason the memcpy work chose CUPTI over interposition.
    // The inference was wrong. Armed from the engine and probed per door on
    // du04 (case 23, PCOMM_TRACE_HOOKS=1), cuGetProcAddress recorded ZERO
    // entries across the whole run, while cupti_buffer_request fired first and
    // succeeded. Resolution is not invocation — the trap PCOMM_TRACE_HOOKS
    // exists for, and it caught this proposal rather than confirming it.
    if (rc == 0 && symbol && pfn && *pfn &&
        std::strcmp(symbol, "cuMemGetHandleForAddressRange") == 0) {
        real_get_handle() = reinterpret_cast<PfnMemGetHandle>(*pfn);
        *pfn = reinterpret_cast<void*>(&get_handle_for_address_range);
    }
    return rc;
}

// Idempotent: registering twice would wrap our own wrapper, which still works
// but makes the chain harder to read than it needs to be.
inline void install() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    pcomm::smart_dlsym::register_symbol(
        "cuGetProcAddress", [](void* next, void*, const char*) -> void* {
            real_get_proc_address() = reinterpret_cast<PfnGetProcAddress>(next);
            return reinterpret_cast<void*>(&get_proc_address);
        });

    pcomm::smart_dlsym::register_symbol(
        "cuMemGetHandleForAddressRange", [](void* next, void*, const char*) -> void* {
            real_get_handle() = reinterpret_cast<PfnMemGetHandle>(next);
            return reinterpret_cast<void*>(&get_handle_for_address_range);
        });
}

}  // namespace pcomm::core::dmabuf
