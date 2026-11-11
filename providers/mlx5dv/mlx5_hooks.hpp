#pragma once
// mlx5dv hook definitions — the per-symbol traits supplying the vendor types
// the engine (comshu::hook) is deliberately free of.
//
// DEFINE_*_HOOK must be invoked at global scope: each expands to a tag struct in
// `namespace mlx5` plus a specialization of ::comshu::hook::HookTrait.
//
// All of these are plain LD_PRELOAD interposers. That is worth stating because
// the CUDA half of IBGDA is *not* reachable that way: NVSHMEM dlopens
// libcuda.so.1 and resolves the driver API with dlsym(handle, ...), which
// searches that library directly and never consults the global scope. Measured
// on du04 — libnvshmem_host.so.3 imports zero cu* symbols and does import
// dlopen/dlsym/dlclose. Everything below, by contrast, is an ordinary dynamic
// symbol and interposes normally, which is why the DEVX control plane needs no
// driver interception at all.

#include <infiniband/mlx5dv.h>

#include "comshu/hook/HookTrait.hpp"

// Registration. Both entry points are hooked, on purpose.
//
// NVSHMEM 3.4.5's IBGDA calls only the _ex variant — measured reg=0, reg_ex=10
// on a live IBGDA run, where hooking the legacy symbol alone logged *zero*
// registrations while six DEVX queue pairs were being created. Older stacks call
// the legacy one. rdma-core may also implement the legacy entry over _ex, which
// would double-count; that is exactly what hooking both ibv_reg_mr and
// ibv_reg_mr_iova2 did in case 01.
//
// So rather than guess which way this rdma-core is wired, both are hooked and
// each row records which entry point it came through (UmemEntry). Double
// counting then shows up as a pair of rows for one buffer — visible in the
// capture and assertable — instead of as a silently inflated total.
DEFINE_PRELOAD_HOOK(mlx5, devx_umem_reg, "mlx5dv_devx_umem_reg", mlx5dv_devx_umem*, ibv_context*,
                    void*, size_t, uint32_t);
DEFINE_PRELOAD_HOOK(mlx5, devx_umem_reg_ex, "mlx5dv_devx_umem_reg_ex", mlx5dv_devx_umem*,
                    ibv_context*, mlx5dv_devx_umem_in*);
DEFINE_PRELOAD_HOOK(mlx5, devx_umem_dereg, "mlx5dv_devx_umem_dereg", int, mlx5dv_devx_umem*);

// The doorbell page. Hooked for its address: knowing which pages are UARs is
// what lets a later stage recognise a doorbell write for what it is.
DEFINE_PRELOAD_HOOK(mlx5, devx_alloc_uar, "mlx5dv_devx_alloc_uar", mlx5dv_devx_uar*, ibv_context*,
                    uint32_t);
//
// free_uar returns void, which the hook engine could not express until the
// HookSigs specialisation and invoke_preload's is_void_v branch landed:
// `bool (*)(void, Args...)` is ill-formed and so is `auto ret = real(...)`.
// SigmaTrace passes std::monostate as a placeholder return type to dodge the
// same wall. Spelling it in the engine means no provider has to learn a
// convention, and a UAR gets a close instead of an open-ended lifetime.
DEFINE_PRELOAD_HOOK(mlx5, devx_free_uar, "mlx5dv_devx_free_uar", void, mlx5dv_devx_uar*);

DEFINE_PRELOAD_HOOK(mlx5, devx_obj_create, "mlx5dv_devx_obj_create", mlx5dv_devx_obj*, ibv_context*,
                    const void*, size_t, void*, size_t);
DEFINE_PRELOAD_HOOK(mlx5, devx_obj_modify, "mlx5dv_devx_obj_modify", int, mlx5dv_devx_obj*,
                    const void*, size_t, void*, size_t);

// Not DEVX, but the same blind spot one layer up: perftest creates its queue
// pairs with mlx5dv_create_qp and never calls ibv_create_qp, which is why case
// 08's ib_queue_pair table has been empty and marked expect_fail.
DEFINE_PRELOAD_HOOK(mlx5, create_qp, "mlx5dv_create_qp", ibv_qp*, ibv_context*,
                    ibv_qp_init_attr_ex*, mlx5dv_qp_init_attr*);

// mlx5dv_create_cq is NOT here, and that is not an oversight. It is hooked by
// the ibverbs provider, because ibv_destroy_cq — which closes what it creates —
// is hooked there, and an entity whose open and close come from two different
// providers is only accounted when the user happens to select both. Exactly one
// of our .so files may define an interposer for a given symbol; this one does
// not define that one. See providers/ibverbs/ib_hooks.hpp.
//
// mlx5dv_create_qp above predates that rule and shows the cost of the other
// choice: its Mlx5QueuePair open lives in this provider's range while
// ibv_destroy_qp's EntityClose comes from ibverbs, so every assertion that
// matches closes to opens has to be told about mlx5_queue_pair by hand.
