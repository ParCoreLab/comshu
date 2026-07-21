#pragma once
// ibverbs hook definitions — the per-symbol traits that supply the vendor
// (ibv_*) types the engine (pcomm::hook) is deliberately free of. Ported from
// SigmaTrace/Ibtrace/IbverbsHooks.hpp, reduced to the phase-1 proof set.
//
// DEFINE_*_HOOK must be invoked at global scope: each expands to a tag struct
// in `namespace ibv` plus a specialization of ::pcomm::hook::HookTrait.

#include <infiniband/verbs.h>

#include "pcomm/hook/HookTrait.hpp"

// --- object-ops hooks (installed by replacing ibv_context->ops.X) ------------
DEFINE_OBJECT_OPS_HOOK(ibv, post_send, "ibv_post_send", int, ibv_qp*, ibv_send_wr*, ibv_send_wr**);
DEFINE_OBJECT_OPS_HOOK_PRED(ibv, poll_cq, "ibv_poll_cq", [](int out_ret, ibv_cq*, int, ibv_wc*) {
    // Skip the empty-poll firehose; record only polls that reaped completions.
    return out_ret > 0;
}, int, ibv_cq*, int, ibv_wc*);

// --- preload hooks (LD_PRELOAD / dlsym interposition) ------------------------
DEFINE_PRELOAD_HOOK(ibv, open_device, "ibv_open_device", ibv_context*, ibv_device*);
DEFINE_PRELOAD_HOOK(ibv, reg_mr, "ibv_reg_mr", ibv_mr*, ibv_pd*, void*, size_t, int);
DEFINE_PRELOAD_HOOK(ibv, dereg_mr, "ibv_dereg_mr", int, ibv_mr*);
DEFINE_PRELOAD_HOOK(ibv, alloc_pd, "ibv_alloc_pd", ibv_pd*, ibv_context*);
DEFINE_PRELOAD_HOOK(ibv, create_qp, "ibv_create_qp", ibv_qp*, ibv_pd*, ibv_qp_init_attr*);
DEFINE_PRELOAD_HOOK(ibv, destroy_qp, "ibv_destroy_qp", int, ibv_qp*);
DEFINE_PRELOAD_HOOK(ibv, modify_qp, "ibv_modify_qp", int, ibv_qp*, ibv_qp_attr*, int);
