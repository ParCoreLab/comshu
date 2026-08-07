#pragma once
// The CUDA driver export-table patcher — the door that reaches a statically
// linked libcudart.
//
// Measured on du04 (NVSHMEM 3.3.24, HPC-SDK 25.9, CUDA 13, A100): NVSHMEM links
// libcudart STATICALLY, so cudaHostGetDevicePointer is not a dynamic symbol to
// interpose, and cudart_static resolves libcuda's cuMemHostGetDevicePointer NOT
// through cuGetProcAddress (which only carried NVSHMEM's own VMM calls) but
// through cuGetExportTable — the private driver export table. Two prior attempts
// (driver API via cuGetProcAddress, runtime API via PLT) both measured
// getdevptr_calls=0. This is the remaining door, and it is exactly the one
// SigmaTrace patches (Profiler/Modules/CudaExportTableOverwrite).
//
// Mechanism, ported from ExportTablePreload.cpp:
//   1. Interpose cuGetExportTable through smart_dlsym (cudart_static reaches it
//      by dlsym on the libcuda handle, which the engine's dlsym interposer sees).
//   2. Call the real one, get the table (an array of function pointers ending in
//      a 0xff..ff sentinel).
//   3. Resolve each registered driver function by name from the now-global
//      libcuda (RTLD_DEFAULT), building real_ptr -> replacement.
//   4. Walk the table; wherever an entry equals a real driver pointer, mprotect
//      its page RW and overwrite it in place with our replacement, keeping the
//      real for the replacement to call through.
//
// This is the most invasive code pcomm carries — it rewrites another library's
// dispatch table in memory. It is gated behind --enable-doorbell-hijack and does
// nothing unless a replacement is registered.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "pcomm/hook/dlsym/Dlsym.hpp"

namespace mlx5::export_table {

using CuResult = int;
// cuGetExportTable(const void** ppExportTable, const CUuuid* pExportTableId).
// The uuid is opaque here — SigmaTrace ignores it too and patches by pointer
// identity, which is robust to which table this is.
using PfnGetExportTable = CuResult (*)(const void**, const void*);

struct Overwrite {
    void* replacement{nullptr};
    void* real{nullptr};
    bool  patched{false};
};

struct State {
    std::mutex                             mutex;
    PfnGetExportTable                      real_get_export_table{nullptr};
    std::unordered_map<std::string, Overwrite> overwrites;
    bool                                   debug{false};
};

inline State& state() {
    static State s;
    static const bool init = [] {
        const char* v = std::getenv("PCOMM_HIJACK_DEBUG");
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
    std::lock_guard<std::mutex> lk(s.mutex);
    s.overwrites[name] = Overwrite{replacement, nullptr, false};
}

// The real driver function for `name`: the one captured while patching, else
// resolved now from the (promoted-global) libcuda. Used by a replacement to call
// through, and to allocate the fake page (cuMemHostAlloc/cuMemFreeHost, which
// are not registered overwrites but resolve the same way).
inline void* real(const std::string& name) {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (auto it = s.overwrites.find(name); it != s.overwrites.end() && it->second.real)
            return it->second.real;
    }
    if (auto* dl = pcomm::smart_dlsym::real_dlsym)
        return dl(pcomm::smart_dlsym::DL_RTLD_DEFAULT, name.c_str());
    return nullptr;
}

// Make libcuda global so RTLD_DEFAULT resolves its driver symbols by name. It is
// already loaded (something asked for its export table); RTLD_NOLOAD promotes
// the existing handle rather than loading a second copy.
inline void promote_libcuda() {
    if (auto* dlopen_fn = pcomm::smart_dlsym::real_dlopen)
        for (const char* name : {"libcuda.so.1", "libcuda.so"})
            if (dlopen_fn(name, pcomm::smart_dlsym::DL_RTLD_NOLOAD |
                                    pcomm::smart_dlsym::DL_RTLD_GLOBAL |
                                    pcomm::smart_dlsym::DL_RTLD_NOW))
                break;
}

// The interposer. Patches every registered driver function found in the table.
inline CuResult get_export_table(const void** pp_table, const void* uuid) {
    auto& s = state();

    PfnGetExportTable real_fn = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        real_fn = s.real_get_export_table;
    }
    if (!real_fn) {  // cannot happen via the registered door; refuse rather than crash
        return 1 /* CUDA_ERROR_INVALID_VALUE */;
    }

    const CuResult rc = real_fn(pp_table, uuid);
    if (rc != 0 || !pp_table || !*pp_table)
        return rc;

    promote_libcuda();

    void** table = static_cast<void**>(const_cast<void*>(*pp_table));

    // Resolve each registered driver function by name; a table entry equal to
    // that pointer is the one to patch. Snapshot under the lock, resolve outside.
    std::vector<std::pair<std::string, void*>> want;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        for (auto& [name, ow] : s.overwrites)
            want.emplace_back(name, ow.replacement);
    }
    std::unordered_map<void*, std::pair<std::string, void*>> real_to_repl;  // real -> (name, repl)
    for (auto& [name, repl] : want) {
        void* r = nullptr;
        if (auto* dl = pcomm::smart_dlsym::real_dlsym)
            r = dl(pcomm::smart_dlsym::DL_RTLD_DEFAULT, name.c_str());
        if (!r) {
            if (s.debug)
                std::fprintf(stderr, "[export_table] %s not found in libcuda\n", name.c_str());
            continue;
        }
        real_to_repl[r] = {name, repl};
        std::lock_guard<std::mutex> lk(s.mutex);
        s.overwrites[name].real = r;  // remember the real so replacements can call through
    }
    if (real_to_repl.empty())
        return rc;

    // Find the table length by its 0xff..ff sentinel, then patch in place.
    const auto sentinel = reinterpret_cast<void*>(~std::uintptr_t{0});
    std::size_t patched = 0, i = 0;
    for (i = 0; table[i] != sentinel; ++i) {
        auto hit = real_to_repl.find(table[i]);
        if (hit == real_to_repl.end())
            continue;
        auto page = reinterpret_cast<std::uintptr_t>(&table[i]) & ~std::uintptr_t{0xFFF};
        if (mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE) != 0)
            continue;  // fail open: leave this entry as the real function
        table[i] = hit->second.second;  // the replacement
        {
            std::lock_guard<std::mutex> lk(s.mutex);
            s.overwrites[hit->second.first].patched = true;
        }
        ++patched;
        if (s.debug)
            std::fprintf(stderr, "[export_table] patched %s at index %zu\n",
                         hit->second.first.c_str(), i);
    }
    if (s.debug)
        std::fprintf(stderr, "[export_table] table entries=%zu patched=%zu\n", i, patched);
    return rc;
}

// Arm the interception. Registers cuGetExportTable with smart_dlsym so
// cudart_static's dlsym for it lands here. Idempotent.
inline void install() {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        static bool                 installed = false;
        if (installed)
            return;
        installed = true;
    }
    pcomm::smart_dlsym::register_symbol(
        "cuGetExportTable", [](void* next, void*, const char*) -> void* {
            state().real_get_export_table = reinterpret_cast<PfnGetExportTable>(next);
            return reinterpret_cast<void*>(&get_export_table);
        });
}

}  // namespace mlx5::export_table
