// Rewriting libcuda's own pointer cells — the door that reaches every route by
// which a driver entry point leaves libcuda. ONE instance per process, here in
// the engine; providers and the engine's own CUDA doors register through the C
// ABI (engine_abi.h, comshu_engine_libcuda_*).
//
// Why this exists: cudart (shared OR statically linked into the application,
// NCCL or NVSHMEM) resolves driver functions through cuGetProcAddress and
// cuGetExportTable, not through dlsym, and copies the pointers it gets. The
// first mechanism interposed cuGetExportTable and patched the table it
// returned — which only works if OUR interposer sees the first fetch. On du04
// (NCCL 2.28.9, CUDA 13) it saw the last of 15: the other 14 had already handed
// NCCL the real cuMemGetHandleForAddressRange, so GIN's dma-buf conversions went
// unseen and none of its puts could be placed.
//
// What libcuda actually does (measured on du04, driver for CUDA 13,
// tests/hpc/probes/libcuda_cells): every driver entry point has EXACTLY ONE
// cell holding its address inside libcuda's writable PT_LOAD segment (the
// RELRO part), the cell is fixed from load time on — it does not move at cuInit
// or at cudart init — and both cudaGetDriverEntryPointByVersion (the static
// cudart route NCCL uses) and cuGetProcAddress_v2 return whatever that cell
// holds. Overwriting the cell before anyone asks makes every one of them hand
// out our replacement, however the asker was linked.
//
// So:
//   1. Find libcuda with dl_iterate_phdr. The scan is bounded by its writable
//      PT_LOAD segments, which is where a pointer needing relocation must live —
//      no sentinel, no table walk, no guess at a table length.
//   2. For each registered replacement, take the real address (dlsym on the
//      libcuda handle) and overwrite every cell equal to it, restoring the
//      page's read-only protection when the cell is in RELRO.
//   3. Do it BEFORE any pointer leaves libcuda. A pointer can only leave through
//      cuGetProcAddress[_v2] or cuGetExportTable, and the asker must dlsym one
//      of those first — through our dlsym interposer, which patches before
//      returning. A libcuda linked as DT_NEEDED is already loaded when the
//      first registrant calls patch(), which patches then.
//
// dlopen is deliberately NOT interposed to catch the load: glibc resolves
// RPATH/$ORIGIN relative to dlopen's caller, and an interposer becomes the
// caller.
//
// WHY IT MOVED HERE FROM providers/mlx5dv (PLAN-20261118-1930). It was a header,
// so every .so including it carried its own State: its own map of what is
// patched and its own idea of what "real" means. Two copies patching different
// names would have worked by luck; two patching the same name would each find
// the other's replacement in the cell instead of the driver's function and
// silently patch nothing — or adopt the other's wrapper as real, which is how a
// wrapper came to call itself on MN5 (job 48812008). And the engine now needs
// this door for the allocation family, which must not depend on whether the
// user selected mlx5dv.
//
// This is the most invasive code comshu carries — it rewrites another library's
// memory. It does nothing unless a replacement is registered.

#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "comshu/engine_abi.h"
#include "comshu/hook/dlsym/Dlsym.hpp"

namespace {

struct Overwrite {
    void*         replacement{nullptr};
    void*         real{nullptr};
    std::uint64_t cells{0};  // how many libcuda cells now hold the replacement
    bool          patched{false};
};

struct State {
    std::recursive_mutex                       mutex;  // patch() may re-enter via dlsym
    std::unordered_map<std::string, Overwrite> overwrites;
    std::atomic<bool>                          pending{false};  // registered, not yet patched
    bool                                       debug{false};
};

State& state() {
    // Leaked, like every engine global: reachable from interposers that run in
    // exit handlers.
    static State* s = [] {
        auto*       st = new State();
        const char* v = std::getenv("COMSHU_HIJACK_DEBUG");
        st->debug = v && *v == '1';
        return st;
    }();
    return *s;
}

// The libcuda handle, by NOLOAD — never loads a second copy and never loads
// libcuda into a process that has not. Cached: libcuda is not unloaded once a
// CUDA context exists, and holding one reference guarantees it.
void* libcuda_handle() {
    static std::atomic<void*> cached{nullptr};
    if (void* h = cached.load(std::memory_order_acquire))
        return h;
    auto* dlopen_fn = comshu::smart_dlsym::real_dlopen;
    if (!dlopen_fn)
        return nullptr;
    for (const char* name : {"libcuda.so.1", "libcuda.so"})
        if (void* h = dlopen_fn(name, comshu::smart_dlsym::DL_RTLD_NOLOAD |
                                          comshu::smart_dlsym::DL_RTLD_NOW)) {
            void* expected = nullptr;
            if (!cached.compare_exchange_strong(expected, h))
                comshu::smart_dlsym::real_dlclose(h);  // lost a race; drop ours
            return cached.load(std::memory_order_acquire);
        }
    return nullptr;
}

struct Segment {
    std::uintptr_t lo, hi;
};
struct LibcudaMap {
    bool                 found{false};
    std::vector<Segment> writable;
    Segment              relro{0, 0};
};

int find_libcuda(struct dl_phdr_info* info, std::size_t, void* out) {
    const char* name = info->dlpi_name;
    if (!name || !std::strstr(name, "/libcuda.so"))
        return 0;
    auto* m = static_cast<LibcudaMap*>(out);
    m->found = true;
    for (int k = 0; k < info->dlpi_phnum; ++k) {
        const auto&          ph = info->dlpi_phdr[k];
        const std::uintptr_t lo = info->dlpi_addr + ph.p_vaddr, hi = lo + ph.p_memsz;
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_W))
            m->writable.push_back({lo, hi});
        else if (ph.p_type == PT_GNU_RELRO)
            m->relro = {lo, hi};
    }
    return 1;
}

// Overwrite libcuda's cells for every registered, not-yet-patched function.
// Returns quickly once nothing is pending. Safe to call from inside dlsym: it
// uses only the real dl* functions and dl_iterate_phdr.
void patch() {
    auto& s = state();
    if (!s.pending.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lk(s.mutex);
    if (!s.pending.load(std::memory_order_relaxed))
        return;

    LibcudaMap m;
    dl_iterate_phdr(&find_libcuda, &m);
    if (!m.found)
        return;  // not loaded yet; the next getter dlsym tries again

    void* handle = libcuda_handle();
    auto* dl = comshu::smart_dlsym::real_dlsym;
    if (!handle || !dl)
        return;

    const long           pagesz = sysconf(_SC_PAGESIZE);
    const std::uintptr_t pmask = ~static_cast<std::uintptr_t>(pagesz - 1);
    for (auto& [name, ow] : s.overwrites) {
        if (ow.patched)
            continue;
        ow.patched = true;  // attempted; a missing symbol stays missing
        void* r = dl(handle, name.c_str());
        if (!r) {
            if (s.debug)
                std::fprintf(stderr, "[libcuda_cells] %s not in libcuda\n", name.c_str());
            continue;
        }
        ow.real = r;
        for (const auto& seg : m.writable) {
            for (std::uintptr_t a = (seg.lo + 7) & ~std::uintptr_t{7}; a + 8 <= seg.hi; a += 8) {
                if (*reinterpret_cast<void* const*>(a) != r)
                    continue;
                void*      page = reinterpret_cast<void*>(a & pmask);
                const bool in_relro = a >= m.relro.lo && a < m.relro.hi;
                if (in_relro && mprotect(page, pagesz, PROT_READ | PROT_WRITE) != 0)
                    continue;  // fail open: this cell keeps the real function
                *reinterpret_cast<void**>(a) = ow.replacement;
                if (in_relro)
                    mprotect(page, pagesz, PROT_READ);
                ++ow.cells;
            }
        }
        if (s.debug)
            std::fprintf(stderr, "[libcuda_cells] %s: %llu cell(s) patched\n", name.c_str(),
                         static_cast<unsigned long long>(ow.cells));
    }
    s.pending.store(false, std::memory_order_release);
}

// Patch before any pointer-getter is handed out. Observe only: the callbacks
// return nullptr, so the dlsym chain for these names is exactly what it was.
void arm_getters() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char* getter : {"cuGetProcAddress", "cuGetProcAddress_v2", "cuGetExportTable"})
            comshu::smart_dlsym::register_symbol(getter, [](void*, void*, const char*) -> void* {
                patch();
                return nullptr;
            });
    });
}

}  // namespace

extern "C" int comshu_engine_libcuda_overwrite(const char* name, void* replacement) {
    if (!name || !replacement)
        return -1;
    auto&                                 s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mutex);
    if (auto it = s.overwrites.find(name); it != s.overwrites.end()) {
        if (it->second.replacement == replacement)
            return 0;  // the same registrant again: idempotent
        std::fprintf(stderr,
                     "[comshu] libcuda cell %s already has an owner; refusing a second "
                     "replacement (two owners of one entry point is a design error)\n",
                     name);
        return -1;
    }
    s.overwrites.emplace(name, Overwrite{replacement, nullptr, 0, false});
    s.pending.store(true, std::memory_order_release);
    arm_getters();
    return 0;
}

extern "C" void* comshu_engine_libcuda_real(const char* name) {
    if (!name)
        return nullptr;
    auto& s = state();
    {
        std::lock_guard<std::recursive_mutex> lk(s.mutex);
        if (auto it = s.overwrites.find(name); it != s.overwrites.end() && it->second.real)
            return it->second.real;
    }
    void* handle = libcuda_handle();
    auto* dl = comshu::smart_dlsym::real_dlsym;
    return handle && dl ? dl(handle, name) : nullptr;
}

extern "C" void comshu_engine_libcuda_patch(void) {
    arm_getters();
    patch();
}

extern "C" uint64_t comshu_engine_libcuda_cells(const char* name) {
    if (!name)
        return 0;
    auto&                                 s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mutex);
    auto                                  it = s.overwrites.find(name);
    return it == s.overwrites.end() ? 0 : it->second.cells;
}
