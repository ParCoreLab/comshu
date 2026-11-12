#pragma once
// Rewriting libcuda's own pointer cells — the door that reaches every route by
// which a driver entry point leaves libcuda.
//
// Why this exists: cudart (shared OR statically linked into NCCL/NVSHMEM)
// resolves driver functions through cuGetProcAddress and cuGetExportTable, not
// through dlsym, and copies the pointers it gets. The previous mechanism
// interposed cuGetExportTable and patched the table it returned — which only
// works if OUR interposer sees the first fetch. On du04 (NCCL 2.28.9, CUDA 13)
// it saw the last of 15: the other 14 had already handed NCCL the real
// cuMemGetHandleForAddressRange, so GIN's dma-buf conversions went unseen and
// none of its puts could be placed. It also found the table's end by walking to
// a 0xff..ff sentinel, and on du04 walked 159,612 entries to get there.
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
//      provider constructor calls install(), which patches then.
//
// dlopen is deliberately NOT interposed to catch the load: glibc resolves
// RPATH/$ORIGIN relative to dlopen's caller, and an interposer becomes the
// caller.
//
// This is the most invasive code comshu carries — it rewrites another library's
// memory. It does nothing unless a replacement is registered.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include "comshu/hook/dlsym/Dlsym.hpp"

namespace mlx5::export_table {

struct Overwrite {
    void*       replacement{nullptr};
    void*       real{nullptr};
    std::size_t cells{0};  // how many libcuda cells now hold the replacement
    bool        patched{false};
};

struct State {
    std::recursive_mutex                       mutex;  // patch() may re-enter via dlsym
    std::unordered_map<std::string, Overwrite> overwrites;
    std::atomic<bool>                          pending{false};  // registered, not yet patched
    bool                                       debug{false};
};

inline State& state() {
    static State      s;
    static const bool init = [] {
        const char* v = std::getenv("COMSHU_HIJACK_DEBUG");
        s.debug = v && *v == '1';
        return true;
    }();
    (void)init;
    return s;
}

// Register a driver function to substitute. Call before install(), from the
// provider constructor when the hijack is enabled.
inline void register_overwrite(const std::string& name, void* replacement) {
    auto&                       s = state();
    std::lock_guard<std::recursive_mutex> lk(s.mutex);
    s.overwrites[name] = Overwrite{replacement, nullptr, 0, false};
    s.pending.store(true, std::memory_order_release);
}

// The real driver function for `name`: the one captured while patching, else
// resolved now from the (promoted-global) libcuda. Used by a replacement to call
// through, and to allocate the fake page (cuMemHostAlloc/cuMemFreeHost, which
// are not registered overwrites but resolve the same way).
inline void* real(const std::string& name) {
    auto& s = state();
    {
        std::lock_guard<std::recursive_mutex> lk(s.mutex);
        if (auto it = s.overwrites.find(name); it != s.overwrites.end() && it->second.real)
            return it->second.real;
    }
    if (auto* dl = comshu::smart_dlsym::real_dlsym)
        return dl(comshu::smart_dlsym::DL_RTLD_DEFAULT, name.c_str());
    return nullptr;
}

// Make libcuda global so RTLD_DEFAULT resolves its driver symbols by name. It is
// already loaded (something asked for its export table); RTLD_NOLOAD promotes
// the existing handle rather than loading a second copy.
inline void promote_libcuda() {
    if (auto* dlopen_fn = comshu::smart_dlsym::real_dlopen)
        for (const char* name : {"libcuda.so.1", "libcuda.so"})
            if (dlopen_fn(name, comshu::smart_dlsym::DL_RTLD_NOLOAD |
                                    comshu::smart_dlsym::DL_RTLD_GLOBAL |
                                    comshu::smart_dlsym::DL_RTLD_NOW))
                break;
}

namespace detail {

struct Segment {
    std::uintptr_t lo, hi;
};
struct LibcudaMap {
    bool                 found{false};
    std::vector<Segment> writable;
    Segment              relro{0, 0};
};

inline int find_libcuda(struct dl_phdr_info* info, std::size_t, void* out) {
    const char* name = info->dlpi_name;
    if (!name || !std::strstr(name, "/libcuda.so"))
        return 0;
    auto* m = static_cast<LibcudaMap*>(out);
    m->found = true;
    for (int k = 0; k < info->dlpi_phnum; ++k) {
        const auto& ph = info->dlpi_phdr[k];
        const std::uintptr_t lo = info->dlpi_addr + ph.p_vaddr, hi = lo + ph.p_memsz;
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_W))
            m->writable.push_back({lo, hi});
        else if (ph.p_type == PT_GNU_RELRO)
            m->relro = {lo, hi};
    }
    return 1;
}

}  // namespace detail

// Overwrite libcuda's cells for every registered, not-yet-patched function.
// Returns quickly once nothing is pending. Safe to call from inside dlsym: it
// uses only the real dl* functions and dl_iterate_phdr.
inline void patch() {
    auto& s = state();
    if (!s.pending.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> lk(s.mutex);
    if (!s.pending.load(std::memory_order_relaxed))
        return;

    detail::LibcudaMap m;
    dl_iterate_phdr(&detail::find_libcuda, &m);
    if (!m.found)
        return;  // not loaded yet; the next getter dlsym tries again

    promote_libcuda();  // real() resolves through RTLD_DEFAULT
    void* handle = nullptr;
    if (auto* dlopen_fn = comshu::smart_dlsym::real_dlopen)
        handle = dlopen_fn("libcuda.so.1", comshu::smart_dlsym::DL_RTLD_NOLOAD |
                                               comshu::smart_dlsym::DL_RTLD_NOW);
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
                std::fprintf(stderr, "[export_table] %s not in libcuda\n", name.c_str());
            continue;
        }
        ow.real = r;
        for (const auto& seg : m.writable) {
            for (std::uintptr_t a = (seg.lo + 7) & ~std::uintptr_t{7}; a + 8 <= seg.hi; a += 8) {
                if (*reinterpret_cast<void* const*>(a) != r)
                    continue;
                void* page = reinterpret_cast<void*>(a & pmask);
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
            std::fprintf(stderr, "[export_table] %s: %zu libcuda cell(s) patched\n",
                         name.c_str(), ow.cells);
    }
    s.pending.store(false, std::memory_order_release);
    comshu::smart_dlsym::real_dlclose(handle);  // drop the NOLOAD reference
}

// Arm the interception: patch now if libcuda is already loaded, and patch
// before any pointer-getter is handed out otherwise. Safe to call repeatedly;
// each call picks up replacements registered since the last.
inline void install() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char* getter : {"cuGetProcAddress", "cuGetProcAddress_v2", "cuGetExportTable"})
            comshu::smart_dlsym::register_symbol(getter, [](void*, void*, const char*) -> void* {
                patch();
                return nullptr;  // observe only; the chain is unchanged
            });
    });
    patch();
}

}  // namespace mlx5::export_table
