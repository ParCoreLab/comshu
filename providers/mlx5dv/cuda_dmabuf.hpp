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
// No CUDA headers: this provider must build where there is no CUDA at all (MN5's
// core configuration does). The types below are hand-copied from cuda.h and
// NOTHING CHECKS THEM — the same exposure that left kAttrDeviceOrdinal wrong for
// the provider's entire life. They are pinned by the typedefs NVSHMEM itself
// uses (src/include/internal/host_transport/cudawrap.h:41,173).

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "pcomm/core/mem_attr.hpp"
#include "pcomm/core/vocab.hpp"
#include "pcomm/hook/dlsym/Dlsym.hpp"

namespace mlx5::dmabuf {

// cuda.h: CUresult is an enum (int); CUdeviceptr is unsigned 64-bit on LP64.
using CuResult = int;
using CuDevicePtr = unsigned long long;

// CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD. The only handle type that yields an fd
// the NIC can import; anything else is not the conversion we are looking for.
inline constexpr int kDmaBufFd = 0x1;

using PfnMemGetHandle = CuResult (*)(void*, CuDevicePtr, std::size_t, int, unsigned long long);
using PfnGetProcAddress = CuResult (*)(const char*, void**, int, unsigned long long);

struct Range {
    std::uint64_t         base{0};
    std::uint64_t         size{0};
    pcomm::core::DeviceId device{pcomm::core::kUnknownDevice};
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
                           pcomm::core::device_of(reinterpret_cast<const void*>(dptr))});
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

}  // namespace mlx5::dmabuf
