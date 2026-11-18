// The CUDA driver's allocation family, observed by the ENGINE, so that a
// cudaMalloc is an object whichever provider ran (PLAN-20261118-1930).
//
// Until this file, plain device allocations were recorded only by gpumem,
// through NVBit — PLAN-20260810-0505 left them out of the engine "until a
// non-gpumem capture needs plain device allocations named". LAMMPS over UCX (MN5
// job 49200469) is that capture: 10,080 halo sends left from GPU buffers, every
// one named by address, and mem_object had zero rows, so none could be called a
// buffer. NVBit costs ~435 s of module lifting to learn an extent one driver call
// returns.
//
// WHY THE DRIVER AND NOT cudaMalloc. nvcc links cudart STATICALLY by default, so
// cudaMalloc crosses no dynamic symbol an interposer could take. Every cudart,
// static or shared, reaches the driver for the actual allocation, and that is
// the narrow waist: cuMemAlloc_v2 and its siblings.
//
// THREE DOORS, because a driver entry point reaches its caller three ways, and
// the tally says which one a run actually took rather than which were armed:
//
//   cell   libcuda's own pointer cell (libcuda_cells.cpp). cuGetProcAddress[_v2]
//          and cuGetExportTable hand out what the cell holds, so this is cudart
//          static or shared, NCCL, NVSHMEM — anything that asks the driver.
//   dlsym  dlsym(<libcuda handle>, "cuMemAlloc_v2"), through the engine's dlsym
//          interposer. Wrapped only when what dlsym found IS libcuda's.
//   link   code linked -lcuda that calls cuMemAlloc_v2 by name: UCX's cuda
//          transports, driver-API applications. The engine is preloaded, so its
//          definitions below win the symbol.
//
// Every wrapper is an OBSERVER. It calls the driver's own function and returns
// exactly what that returned; the only thing it adds is a record. A depth guard
// makes the wrappers transparent to each other: if one door's wrapper is
// reached from inside another's (a dlsym of the default scope finds the link
// definition; the driver calling one entry point through another's cell), the
// inner one passes straight through, so one allocation is one record.
//
// No CUDA header, by the engine's invariant: the types are spelled out below
// and pinned by cuda.h's own definitions (CUresult is an int enum; CUdeviceptr
// is unsigned 64-bit on LP64; streams and pools are opaque pointers; a VMM
// generic handle is unsigned long long).
//
// COMSHU_ALLOC_DOORS=0 registers neither the cell nor the dlsym door and turns
// the link definitions into pure pass-throughs. COMSHU_TRACE_ALLOC=1 prints
// every observed call with its door.

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "comshu/core/vocab.hpp"
#include "comshu/engine_abi.h"
#include "comshu/hook/dlsym/Dlsym.hpp"
#include "comshu/tracer.hpp"
#include "engine_objects.hpp"

namespace {

using CuResult = int;
using CuDevicePtr = unsigned long long;
using CuStream = void*;
using CuPool = void*;
using CuMemHandle = unsigned long long;

constexpr CuResult kSuccess = 0;
constexpr CuResult kNotInitialized = 3;  // CUDA_ERROR_NOT_INITIALIZED

enum Door : std::uint8_t { kCell = 0, kDlsym = 1, kLink = 2, kDoorCount = 3 };
constexpr const char* kDoorName[kDoorCount] = {"cell", "dlsym", "link"};

// The entry points, in one table. The names are the driver's EXPORTED names —
// the versioned ones (_v2) that cuda.h's macros map the plain spellings onto,
// and the per-thread-default-stream (_ptsz) variants that a build with
// --default-stream per-thread reaches instead.
enum Api : std::uint8_t {
    kMemAlloc,
    kMemAllocPitch,
    kMemAllocManaged,
    kMemAllocAsync,
    kMemAllocAsyncPtsz,
    kMemAllocFromPoolAsync,
    kMemAllocFromPoolAsyncPtsz,
    kMemAllocHost,
    kMemHostAlloc,
    kMemMap,
    kMemFree,
    kMemFreeAsync,
    kMemFreeAsyncPtsz,
    kMemFreeHost,
    kMemUnmap,
    kApiCount
};
constexpr const char* kName[kApiCount] = {
    "cuMemAlloc_v2",
    "cuMemAllocPitch_v2",
    "cuMemAllocManaged",
    "cuMemAllocAsync",
    "cuMemAllocAsync_ptsz",
    "cuMemAllocFromPoolAsync",
    "cuMemAllocFromPoolAsync_ptsz",
    "cuMemAllocHost_v2",
    "cuMemHostAlloc",
    "cuMemMap",
    "cuMemFree_v2",
    "cuMemFreeAsync",
    "cuMemFreeAsync_ptsz",
    "cuMemFreeHost",
    "cuMemUnmap",
};

bool doors_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_ALLOC_DOORS");
        return !(v && *v == '0');
    }();
    return on;
}

bool trace_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_TRACE_ALLOC");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

// The driver's function per entry point, resolved once. comshu_engine_libcuda_real
// answers with the function the cell patch displaced, or the handle-scoped
// symbol — libcuda's own in both cases, never one of these wrappers.
std::atomic<void*> g_real[kApiCount];

template<typename Fn> Fn real(Api a) {
    void* p = g_real[a].load(std::memory_order_acquire);
    if (!p) {
        p = comshu_engine_libcuda_real(kName[a]);
        if (p)
            g_real[a].store(p, std::memory_order_release);
    }
    return reinterpret_cast<Fn>(p);
}

// Calls per (door, entry point), and how many became records. Plain relaxed
// atomics: allocation calls are rare next to everything else comshu counts.
std::atomic<std::uint64_t> g_calls[kDoorCount][kApiCount];
// Calls from comshu's own code, passed through unrecorded. Counted so a run
// shows how much it excluded rather than excluding silently.
std::atomic<std::uint64_t> g_own{0};

// `outer` is the decision to record: the outermost wrapper on this thread,
// called from code that is not comshu's own. The caller is the wrapper's return
// address, which is why every wrapper constructs its Guard from
// __builtin_return_address(0) itself rather than in a helper.
thread_local int t_depth = 0;
struct Guard {
    bool outer;
    explicit Guard(const void* caller) : outer(t_depth++ == 0) {
        if (outer && comshu::engine::is_comshu_code(caller)) {
            outer = false;
            g_own.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ~Guard() { --t_depth; }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};
// The caller as the LINK door saw it. A cell or dlsym wrapper is the function
// the application called through a pointer, so its own return address is the
// caller. A link-door export is a different function that calls the shared
// template, and the template's return address would then be the export — engine
// code, which is_comshu_code would rightly call ours, dropping every -lcuda
// allocation. So the export captures its return address here first.
thread_local const void* t_link_caller = nullptr;
struct LinkCaller {
    explicit LinkCaller(const void* pc) { t_link_caller = pc; }
    ~LinkCaller() { t_link_caller = nullptr; }  // never leaks into the next call
};
#define COMSHU_ALLOC_GUARD                                                                         \
    Guard g(t_link_caller ? t_link_caller : __builtin_return_address(0));                          \
    t_link_caller = nullptr
#define COMSHU_LINK_CALLER LinkCaller link_caller_(__builtin_return_address(0))

void tally(Door d, Api a, std::uint64_t addr, std::uint64_t size) {
    g_calls[d][a].fetch_add(1, std::memory_order_relaxed);
    if (trace_enabled())
        std::fprintf(stderr, "[comshu-alloc] door=%s %s addr=0x%llx size=%llu\n", kDoorName[d],
                     kName[a], static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(size));
}

void opened(Door d, Api a, std::uint64_t base, std::uint64_t size, comshu::core::ObjectOrigin o,
            int known_device = -1) {
    tally(d, a, base, size);
    comshu::engine::note_object(base, size, o, known_device);
}

// The device a VMM allocation handle was created on, or -1.
//
// Asked of the HANDLE, not of the address, because at cuMemMap time the address
// usually cannot answer: access is granted by cuMemSetAccess AFTER the map (that
// is the order NCCL, NVSHMEM and PyTorch all use), and a pointer query on a
// mapping with no access yet is a failure that would be recorded as
// AttrStatus::OrdinalFailed — a real defect report about a healthy buffer.
//
// cuda.h's CUmemAllocationProp, hand-copied as the engine's invariant requires:
// {int type; int requestedHandleTypes; {int type; int id} location; ...}. Only
// the location is read, at bytes 8..15, and the struct is given generous zeroed
// room so a driver writing a longer one cannot overrun it.
// CU_MEM_LOCATION_TYPE_DEVICE = 1.
int device_of_vmm_handle(CuMemHandle h) {
    using Pfn = CuResult (*)(void*, CuMemHandle);
    static Pfn fn =
        reinterpret_cast<Pfn>(comshu_engine_libcuda_real("cuMemGetAllocationPropertiesFromHandle"));
    if (!fn)
        return -1;
    alignas(8) unsigned char prop[128] = {};
    if (fn(prop, h) != kSuccess)
        return -1;
    int loc_type = 0, loc_id = -1;
    std::memcpy(&loc_type, prop + 8, sizeof loc_type);
    std::memcpy(&loc_id, prop + 12, sizeof loc_id);
    return loc_type == 1 && loc_id >= 0 ? loc_id : -1;
}

void closing(Door d, Api a, std::uint64_t base) {
    tally(d, a, base, 0);
    comshu::engine::close_object(base);
}

using comshu::core::ObjectOrigin;

// --- the wrappers, one template per entry point, one instantiation per door --
//
// Allocations record AFTER the driver succeeds (there is no address before).
// Frees close BEFORE the driver runs, as nvshmem_free does: once it returns, the
// address can already belong to a new allocation, and the other order would let
// that new object be registered and then closed. A free the driver then rejects
// has still closed the object; a free of a pointer that is not a live object
// closes nothing (engine_objects.hpp).

template<Door D> CuResult mem_alloc(CuDevicePtr* dptr, std::size_t bytes) {
    auto* f = real<CuResult (*)(CuDevicePtr*, std::size_t)>(kMemAlloc);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(dptr, bytes);
    if (g.outer && rc == kSuccess && dptr && doors_enabled())
        opened(D, kMemAlloc, *dptr, bytes, ObjectOrigin::CuMemAlloc);
    return rc;
}

template<Door D>
CuResult mem_alloc_pitch(CuDevicePtr* dptr, std::size_t* pitch, std::size_t width,
                         std::size_t height, unsigned elem) {
    auto* f = real<CuResult (*)(CuDevicePtr*, std::size_t*, std::size_t, std::size_t, unsigned)>(
        kMemAllocPitch);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(dptr, pitch, width, height, elem);
    // The extent is pitch * height: the driver pads every row to `pitch`, and
    // the padding is part of the allocation an address can fall in.
    if (g.outer && rc == kSuccess && dptr && pitch && doors_enabled())
        opened(D, kMemAllocPitch, *dptr, *pitch * height, ObjectOrigin::CuMemAlloc);
    return rc;
}

template<Door D> CuResult mem_alloc_managed(CuDevicePtr* dptr, std::size_t bytes, unsigned flags) {
    auto* f = real<CuResult (*)(CuDevicePtr*, std::size_t, unsigned)>(kMemAllocManaged);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(dptr, bytes, flags);
    if (g.outer && rc == kSuccess && dptr && doors_enabled())
        opened(D, kMemAllocManaged, *dptr, bytes, ObjectOrigin::CuMemAllocManaged);
    return rc;
}

template<Door D, Api A> CuResult mem_alloc_async(CuDevicePtr* dptr, std::size_t bytes, CuStream s) {
    auto* f = real<CuResult (*)(CuDevicePtr*, std::size_t, CuStream)>(A);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(dptr, bytes, s);
    if (g.outer && rc == kSuccess && dptr && doors_enabled())
        opened(D, A, *dptr, bytes, ObjectOrigin::CuMemAllocAsync);
    return rc;
}

template<Door D, Api A>
CuResult mem_alloc_from_pool_async(CuDevicePtr* dptr, std::size_t bytes, CuPool pool, CuStream s) {
    auto* f = real<CuResult (*)(CuDevicePtr*, std::size_t, CuPool, CuStream)>(A);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(dptr, bytes, pool, s);
    if (g.outer && rc == kSuccess && dptr && doors_enabled())
        opened(D, A, *dptr, bytes, ObjectOrigin::CuMemAllocAsync);
    return rc;
}

template<Door D> CuResult mem_alloc_host(void** pp, std::size_t bytes) {
    auto* f = real<CuResult (*)(void**, std::size_t)>(kMemAllocHost);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(pp, bytes);
    if (g.outer && rc == kSuccess && pp && doors_enabled())
        opened(D, kMemAllocHost, reinterpret_cast<std::uint64_t>(*pp), bytes,
               ObjectOrigin::CuMemHost);
    return rc;
}

template<Door D> CuResult mem_host_alloc(void** pp, std::size_t bytes, unsigned flags) {
    auto* f = real<CuResult (*)(void**, std::size_t, unsigned)>(kMemHostAlloc);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(pp, bytes, flags);
    if (g.outer && rc == kSuccess && pp && doors_enabled())
        opened(D, kMemHostAlloc, reinterpret_cast<std::uint64_t>(*pp), bytes,
               ObjectOrigin::CuMemHost);
    return rc;
}

// A VMM mapping is the object: cuMemCreate makes a handle with no address, and
// cuMemAddressReserve makes an address range with no memory behind it. Only the
// map joins them, and one reservation may hold many maps (PyTorch's expandable
// segments grow one reservation chunk by chunk), each its own object.
template<Door D>
CuResult mem_map(CuDevicePtr ptr, std::size_t size, std::size_t offset, CuMemHandle h,
                 unsigned long long flags) {
    auto* f =
        real<CuResult (*)(CuDevicePtr, std::size_t, std::size_t, CuMemHandle, unsigned long long)>(
            kMemMap);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    const CuResult rc = f(ptr, size, offset, h, flags);
    if (g.outer && rc == kSuccess && doors_enabled())
        opened(D, kMemMap, ptr, size, ObjectOrigin::CuMemMap, device_of_vmm_handle(h));
    return rc;
}

template<Door D> CuResult mem_free(CuDevicePtr p) {
    auto* f = real<CuResult (*)(CuDevicePtr)>(kMemFree);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    if (g.outer && doors_enabled())
        closing(D, kMemFree, p);
    return f(p);
}

// Closed at the CALL, though the memory returns to the pool only when the
// stream reaches the free. The call is the last moment the program can name
// the buffer, and a later cuMemAllocAsync on the same stream may be handed the
// same address while the GPU has not yet run the free — the [open, close)
// window is in call order, as ObjectOrigin::CuMemAllocAsync says.
template<Door D, Api A> CuResult mem_free_async(CuDevicePtr p, CuStream s) {
    auto* f = real<CuResult (*)(CuDevicePtr, CuStream)>(A);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    if (g.outer && doors_enabled())
        closing(D, A, p);
    return f(p, s);
}

template<Door D> CuResult mem_free_host(void* p) {
    auto* f = real<CuResult (*)(void*)>(kMemFreeHost);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    if (g.outer && doors_enabled())
        closing(D, kMemFreeHost, reinterpret_cast<std::uint64_t>(p));
    return f(p);
}

// One unmap may cover several mappings; each one inside the range is closed.
template<Door D> CuResult mem_unmap(CuDevicePtr ptr, std::size_t size) {
    auto* f = real<CuResult (*)(CuDevicePtr, std::size_t)>(kMemUnmap);
    if (!f)
        return kNotInitialized;
    COMSHU_ALLOC_GUARD;
    if (g.outer && doors_enabled()) {
        tally(D, kMemUnmap, ptr, size);
        comshu::engine::close_objects_in(ptr, ptr + size);
    }
    return f(ptr, size);
}

// The wrapper a door hands out for entry point `a`.
template<Door D> void* wrapper(Api a) {
    switch (a) {
        case kMemAlloc:
            return reinterpret_cast<void*>(&mem_alloc<D>);
        case kMemAllocPitch:
            return reinterpret_cast<void*>(&mem_alloc_pitch<D>);
        case kMemAllocManaged:
            return reinterpret_cast<void*>(&mem_alloc_managed<D>);
        case kMemAllocAsync:
            return reinterpret_cast<void*>(&mem_alloc_async<D, kMemAllocAsync>);
        case kMemAllocAsyncPtsz:
            return reinterpret_cast<void*>(&mem_alloc_async<D, kMemAllocAsyncPtsz>);
        case kMemAllocFromPoolAsync:
            return reinterpret_cast<void*>(&mem_alloc_from_pool_async<D, kMemAllocFromPoolAsync>);
        case kMemAllocFromPoolAsyncPtsz:
            return reinterpret_cast<void*>(
                &mem_alloc_from_pool_async<D, kMemAllocFromPoolAsyncPtsz>);
        case kMemAllocHost:
            return reinterpret_cast<void*>(&mem_alloc_host<D>);
        case kMemHostAlloc:
            return reinterpret_cast<void*>(&mem_host_alloc<D>);
        case kMemMap:
            return reinterpret_cast<void*>(&mem_map<D>);
        case kMemFree:
            return reinterpret_cast<void*>(&mem_free<D>);
        case kMemFreeAsync:
            return reinterpret_cast<void*>(&mem_free_async<D, kMemFreeAsync>);
        case kMemFreeAsyncPtsz:
            return reinterpret_cast<void*>(&mem_free_async<D, kMemFreeAsyncPtsz>);
        case kMemFreeHost:
            return reinterpret_cast<void*>(&mem_free_host<D>);
        case kMemUnmap:
            return reinterpret_cast<void*>(&mem_unmap<D>);
        case kApiCount:
            break;
    }
    return nullptr;
}

// Is `fn` libcuda's own code? The dlsym door wraps only the driver's function.
// Anything else is the link definition below (the default scope found the
// engine first — it is already observed) or a third party's interposer, and is
// left exactly where it is: the rule core/dmabuf_registry.hpp's is_driver_fn
// learned from MN5 job 48812008.
bool is_libcuda(const void* fn) {
    Dl_info info{};
    return fn && dladdr(fn, &info) && info.dli_fname && std::strstr(info.dli_fname, "libcuda");
}

// One line at exit, only when something was observed: which door the
// allocations actually came through. "Armed" is not evidence; this is.
void report() {
    std::uint64_t per_door[kDoorCount] = {0, 0, 0};
    for (int d = 0; d < kDoorCount; ++d)
        for (int a = 0; a < kApiCount; ++a)
            per_door[d] += g_calls[d][a].load(std::memory_order_relaxed);
    const std::uint64_t own = g_own.load(std::memory_order_relaxed);
    if (!(per_door[kCell] | per_door[kDlsym] | per_door[kLink] | own))
        return;
    std::fprintf(stderr,
                 "[comshu pid %d] cuda allocation doors: cell=%llu dlsym=%llu link=%llu calls, "
                 "comshu's own=%llu (not recorded)%s\n",
                 static_cast<int>(getpid()), static_cast<unsigned long long>(per_door[kCell]),
                 static_cast<unsigned long long>(per_door[kDlsym]),
                 static_cast<unsigned long long>(per_door[kLink]),
                 static_cast<unsigned long long>(own),
                 comshu::Tracer::instance().has_recorder() ? "" : " (no capture running)");
}

// Armed when the engine loads, AFTER the dlsym interposer's own constructor
// (priority 0) has built its registry: this one has no priority, so it runs
// later within this object. Early is the point — the cell must be patched
// before cudart asks for its first pointer, and the getter dlsym that triggers
// the patch can come before main.
__attribute__((constructor)) void arm_alloc_doors() {
    if (!doors_enabled())
        return;
    for (int a = 0; a < kApiCount; ++a) {
        const auto api = static_cast<Api>(a);
        comshu_engine_libcuda_overwrite(kName[a], wrapper<kCell>(api));
        comshu::smart_dlsym::register_symbol(
            kName[a], [api](void* next, void*, const char*) -> void* {
                return is_libcuda(next) ? wrapper<kDlsym>(api) : nullptr;
            });
    }
    comshu_engine_libcuda_patch();  // now, if libcuda is already loaded (-lcuda)
    std::atexit(report);
}

}  // namespace

// --- the link door ------------------------------------------------------------
//
// The engine is preloaded, so these definitions win the symbol for every caller
// linked -lcuda. They are exported unconditionally (a symbol cannot be
// withdrawn at run time); with COMSHU_ALLOC_DOORS=0 they only forward.
//
// One consequence is deliberate and load-bearing: dlsym(RTLD_DEFAULT,
// "cuMemAlloc_v2") now finds THIS definition. Every comshu path that needs the
// driver itself goes through comshu_engine_libcuda_real, which resolves in the
// libcuda handle's own scope, and the dlsym door refuses to wrap anything that
// is not libcuda's — so a default-scope lookup gets the link wrapper, observed
// once, and nothing calls itself.

extern "C" {
CuResult cuMemAlloc_v2(CuDevicePtr* dptr, std::size_t bytes) {
    COMSHU_LINK_CALLER;
    return mem_alloc<kLink>(dptr, bytes);
}
CuResult cuMemAllocPitch_v2(CuDevicePtr* dptr, std::size_t* pitch, std::size_t width,
                            std::size_t height, unsigned elem) {
    COMSHU_LINK_CALLER;
    return mem_alloc_pitch<kLink>(dptr, pitch, width, height, elem);
}
CuResult cuMemAllocManaged(CuDevicePtr* dptr, std::size_t bytes, unsigned flags) {
    COMSHU_LINK_CALLER;
    return mem_alloc_managed<kLink>(dptr, bytes, flags);
}
CuResult cuMemAllocAsync(CuDevicePtr* dptr, std::size_t bytes, CuStream s) {
    COMSHU_LINK_CALLER;
    return mem_alloc_async<kLink, kMemAllocAsync>(dptr, bytes, s);
}
CuResult cuMemAllocAsync_ptsz(CuDevicePtr* dptr, std::size_t bytes, CuStream s) {
    COMSHU_LINK_CALLER;
    return mem_alloc_async<kLink, kMemAllocAsyncPtsz>(dptr, bytes, s);
}
CuResult cuMemAllocFromPoolAsync(CuDevicePtr* dptr, std::size_t bytes, CuPool pool, CuStream s) {
    COMSHU_LINK_CALLER;
    return mem_alloc_from_pool_async<kLink, kMemAllocFromPoolAsync>(dptr, bytes, pool, s);
}
CuResult cuMemAllocFromPoolAsync_ptsz(CuDevicePtr* dptr, std::size_t bytes, CuPool pool,
                                      CuStream s) {
    COMSHU_LINK_CALLER;
    return mem_alloc_from_pool_async<kLink, kMemAllocFromPoolAsyncPtsz>(dptr, bytes, pool, s);
}
CuResult cuMemAllocHost_v2(void** pp, std::size_t bytes) {
    COMSHU_LINK_CALLER;
    return mem_alloc_host<kLink>(pp, bytes);
}
CuResult cuMemHostAlloc(void** pp, std::size_t bytes, unsigned flags) {
    COMSHU_LINK_CALLER;
    return mem_host_alloc<kLink>(pp, bytes, flags);
}
CuResult cuMemMap(CuDevicePtr ptr, std::size_t size, std::size_t offset, CuMemHandle h,
                  unsigned long long flags) {
    COMSHU_LINK_CALLER;
    return mem_map<kLink>(ptr, size, offset, h, flags);
}
CuResult cuMemFree_v2(CuDevicePtr p) {
    COMSHU_LINK_CALLER;
    return mem_free<kLink>(p);
}
CuResult cuMemFreeAsync(CuDevicePtr p, CuStream s) {
    COMSHU_LINK_CALLER;
    return mem_free_async<kLink, kMemFreeAsync>(p, s);
}
CuResult cuMemFreeAsync_ptsz(CuDevicePtr p, CuStream s) {
    COMSHU_LINK_CALLER;
    return mem_free_async<kLink, kMemFreeAsyncPtsz>(p, s);
}
CuResult cuMemFreeHost(void* p) {
    COMSHU_LINK_CALLER;
    return mem_free_host<kLink>(p);
}
CuResult cuMemUnmap(CuDevicePtr ptr, std::size_t size) {
    COMSHU_LINK_CALLER;
    return mem_unmap<kLink>(ptr, size);
}
}  // extern "C"
