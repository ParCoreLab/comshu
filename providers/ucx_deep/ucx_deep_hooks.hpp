#pragma once
// ONE interposed symbol. Everything else this provider hooks is a function
// pointer inside a struct, not a symbol, which is the whole point — see
// ucx_deep_records.hpp and PLAN-20260827-1449.
//
// `uct_iface_open` is exported by libuct.so.0 and is an UND reference in
// libucp.so.0 (verified with nm -D against MN5's HPC-X 2.22.1 tree), so it
// binds to us under LD_PRELOAD like any other door. It hands back the
// uct_iface_h whose `ops` table carries the entire UCT data plane.

#include <uct/api/uct.h>

#include "pcomm/hook/HookTrait.hpp"

DEFINE_PRELOAD_HOOK(uctd, iface_open, "uct_iface_open", ucs_status_t, uct_md_h, uct_worker_h,
                    const uct_iface_params_t*, const uct_iface_config_t*, uct_iface_h*);
