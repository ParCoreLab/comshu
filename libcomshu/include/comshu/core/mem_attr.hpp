#pragma once
// L0 — pointer -> owning device, for any provider that holds an address.
//
// This lived inside mlx5dv_provider.cpp until ibverbs needed the same answer.
// Extracting it *before* the second copy existed is the whole reason it is
// here: the first copy shipped with kAttrDeviceOrdinal = 4 — that is
// CU_POINTER_ATTRIBUTE_HOST_POINTER, not DEVICE_ORDINAL — and the mistake
// survived for the provider's entire life because every failure mode of this
// function looks identical from the capture (device = kUnknownDevice). Two
// copies would have meant two places for that to happen again.
//
// Deliberately header-only and deliberately NOT linked against CUDA. It must
// compile and run on a host with no CUDA at all — MN5's core configuration is
// exactly that, and the suite has to stay green there. Where the driver is
// absent every address is simply reported as kUnknownDevice.
//
// Note what this does NOT do: it answers "which GPU owns this allocation", not
// "which GPU touched this byte". Per-access attribution is a different question
// and still belongs to the NVBit engine.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>

#include "comshu/core/vocab.hpp"

namespace comshu::core {

// The libcuda entry point, by signature rather than by header. Declaring it
// ourselves is what keeps this translation unit free of cuda.h.
using CuPointerGetAttributeFn = int (*)(void*, int, unsigned long long);

// These are hand-copied from cuda.h because nothing here links CUDA, which
// means the compiler cannot check them. Keep that in mind before editing one:
// the last wrong constant here cost a provider's entire lifetime of wrong
// device ordinals, and nothing downstream could tell.
inline constexpr int kAttrMemoryType = 2;     // CU_POINTER_ATTRIBUTE_MEMORY_TYPE
inline constexpr int kAttrDeviceOrdinal = 9;  // CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL
inline constexpr int kMemoryTypeDevice = 2;   // CU_MEMORYTYPE_DEVICE

// RTLD_NOLOAD on purpose: this must never *load* libcuda into a process that
// did not already have it, only borrow it from one that did. A hook that
// manufactures a capability the application declined is the devx_probe failure,
// and it is not worth a device ordinal.
//
// The result is cached once RESOLVED and deliberately not cached when it is
// not. An earlier version latched on first call — `static fn = [...]()` runs
// exactly once — so a single early miss made attribution permanently dead.
inline CuPointerGetAttributeFn cu_pointer_get_attribute() {
    static CuPointerGetAttributeFn cached = nullptr;
    if (cached)
        return cached;
    void* lib = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!lib)
        return nullptr;
    cached = reinterpret_cast<CuPointerGetAttributeFn>(dlsym(lib, "cuPointerGetAttribute"));
    return cached;
}

// PROBE, gated on COMSHU_TRACE_HOOKS=1 — prints the FIRST attribution attempt
// only. kUnknownDevice is ambiguous by construction: it means "host memory"
// *and* "CUDA is not present" *and* "the call failed". Those three imply
// completely different work and look identical in the capture, which is exactly
// what hid the wrong-constant bug. This is the only thing that tells them
// apart, so it stays.
inline void trace_device_attr(const char* stage, int rc, unsigned int type) {
    static bool first = true;
    if (!first)
        return;
    const char* on = std::getenv("COMSHU_TRACE_HOOKS");
    if (!on || !*on || std::strcmp(on, "0") == 0)
        return;
    first = false;
    std::fprintf(stderr, "[comshu-trace] device_of: stage=%s rc=%d type=%u\n", stage, rc, type);
    std::fflush(stderr);
}

// The owning device of an address AND why it is what it is. Every return site
// names its own AttrStatus, which is the point: kUnknownDevice comes back from
// five of them, and a MemObject records which.
struct Attribution {
    DeviceId   device = kUnknownDevice;
    AttrStatus status = AttrStatus::NotQueried;
};

inline Attribution attribute(const void* addr) {
    if (!addr) {
        trace_device_attr("null-addr", -1, 0);
        return {kUnknownDevice, AttrStatus::NullAddr};
    }
    auto* fn = cu_pointer_get_attribute();
    if (!fn) {
        trace_device_attr("unresolved", -1, 0);
        return {kUnknownDevice, AttrStatus::DriverAbsent};
    }
    unsigned int type = 0;
    const int    rc = fn(&type, kAttrMemoryType, reinterpret_cast<unsigned long long>(addr));
    trace_device_attr("called", rc, type);
    if (rc != 0)
        return {kUnknownDevice, AttrStatus::HostNotCuda};
    if (static_cast<int>(type) != kMemoryTypeDevice)
        return {kUnknownDevice, AttrStatus::HostCudaKnown};
    int ordinal = -1;
    if (fn(&ordinal, kAttrDeviceOrdinal, reinterpret_cast<unsigned long long>(addr)) != 0 ||
        ordinal < 0)
        return {kUnknownDevice, AttrStatus::OrdinalFailed};
    return {static_cast<DeviceId>(ordinal), AttrStatus::Device};
}

// The device alone, for records that carry no status (UCX's, Mlx5Umem). A
// MemObject emitter uses attribute(): that record is where the reason lives.
inline DeviceId device_of(const void* addr) { return attribute(addr).device; }

// The status to record beside a device some PRODUCER named (gpumem, the dma-buf
// registry) rather than one asked for here.
inline AttrStatus status_of_named(DeviceId device) {
    return device == kUnknownDevice ? AttrStatus::NotQueried : AttrStatus::Device;
}

}  // namespace comshu::core
