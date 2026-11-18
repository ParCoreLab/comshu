#pragma once
// mlx5dv's spelling of the engine's libcuda cell patcher.
//
// The patcher itself — finding libcuda's writable segments, overwriting each
// entry point's one cell before any pointer-getter is handed out, and why that
// reaches cudart static, cudart shared, NCCL and NVSHMEM alike — lives in
// libcomshu/src/libcuda_cells.cpp, with one instance per process. It was this
// header until PLAN-20261118-1930: header-only meant one copy of its state per
// .so that included it, and the engine now needs the same door for the CUDA
// allocation family, which must not depend on mlx5dv being selected.
//
// What is left here is the old interface, so uar_hijack.hpp, host_doorbell.hpp
// and dmabuf_door.hpp read exactly as they did.

#include <cstdio>
#include <string>

#include "comshu/engine_abi.h"
#include "comshu/hook/dlsym/Dlsym.hpp"

namespace mlx5::export_table {

// Register a driver function to substitute. A refusal (another owner already
// holds that entry point) is reported by the engine; the cell then keeps the
// other owner's replacement and this one never runs.
inline void register_overwrite(const std::string& name, void* replacement) {
    (void)comshu_engine_libcuda_overwrite(name.c_str(), replacement);
}

// The driver's own function for `name`: the one the patch displaced, else
// resolved through the libcuda handle — never RTLD_DEFAULT, which now names the
// engine's own definitions for the allocation family. That matters here: the
// UAR hijack allocates its fake page with cuMemHostAlloc through this, and must
// reach the driver, not the engine's observer, or comshu's own page would be
// recorded as an application object.
inline void* real(const std::string& name) {
    return comshu_engine_libcuda_real(name.c_str());
}

// Make libcuda global, for code that resolves driver symbols by name through
// the default scope. RTLD_NOLOAD promotes the existing handle rather than
// loading a second copy.
inline void promote_libcuda() {
    if (auto* dlopen_fn = comshu::smart_dlsym::real_dlopen)
        for (const char* name : {"libcuda.so.1", "libcuda.so"})
            if (dlopen_fn(name, comshu::smart_dlsym::DL_RTLD_NOLOAD |
                                    comshu::smart_dlsym::DL_RTLD_GLOBAL |
                                    comshu::smart_dlsym::DL_RTLD_NOW))
                break;
}

// Arm the interception: patch now if libcuda is already loaded, and before any
// pointer-getter is handed out otherwise. Idempotent.
inline void install() {
    comshu_engine_libcuda_patch();
}

}  // namespace mlx5::export_table
