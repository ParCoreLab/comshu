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
// Extended queue-pair creation. This is an object-ops hook and not a preload
// hook because **there is no exported ibv_create_qp_ex symbol to preload**:
//
//   $ readelf --dyn-syms /lib64/libibverbs.so.1 | grep create_qp
//     ibv_create_qp@@IBVERBS_1.1
//     ibv_create_qp@IBVERBS_1.0
//
// verbs.h makes ibv_create_qp_ex a static inline that dispatches through
// verbs_context::create_qp_ex (verbs.h:3071-3085), so the function pointer is
// the only thing there is to hook — the same shape as post_send.
//
// This cost case 08 a week of wrong answers, so the method is worth recording:
// three explanations were asserted from `nm` on the binary and LD_DEBUG=bindings
// on the run, and all three failed the same way. Both instruments answer
// questions about *symbols*, and a static inline has no symbol. What settled it
// was tracing hook **entry** (PCOMM_TRACE_HOOKS=1, job 44233994): every hooked
// door reported, ibv_modify_qp and ibv_qp_to_qp_ex among them, and no create
// door at all. A queue pair that is modified but never created cannot have come
// through an exported symbol.
DEFINE_OBJECT_OPS_HOOK(ibv, create_qp_ex, "ibv_create_qp_ex", ibv_qp*, ibv_context*,
                       ibv_qp_init_attr_ex*);
DEFINE_OBJECT_OPS_HOOK_PRED(
    ibv, poll_cq, "ibv_poll_cq",
    [](int out_ret, ibv_cq*, int, ibv_wc*) {
        // Skip the empty-poll firehose; record only polls that reaped completions.
        return out_ret > 0;
    },
    int, ibv_cq*, int, ibv_wc*);

// --- preload hooks (LD_PRELOAD / dlsym interposition) ------------------------
DEFINE_PRELOAD_HOOK(ibv, open_device, "ibv_open_device", ibv_context*, ibv_device*);
// Memory registration, hooked at the one place every path funnels through.
//
// verbs.h makes `ibv_reg_mr` a macro over a static inline that calls the legacy
// exported symbol only when the access flags are a compile-time constant with no
// IBV_ACCESS_OPTIONAL_RANGE bit, and `ibv_reg_mr_iova2` otherwise — so hooking
// only the legacy symbol captured literal-flag callers (ibv_rc_pingpong) and
// missed ordinary ones (perftest, which builds its flags from options).
//
// Hooking *both* double-counts, which is not obvious from the header and was
// measured: rdma-core implements the legacy `ibv_reg_mr` on top of
// `ibv_reg_mr_iova2`, and because our interposers are global that internal call
// lands on ours too. Case 01 went from one region per rank to two identical
// rows — same mr, same base, same lkey.
//
// So only iova2 is hooked. Both application-visible paths reach it, which cases
// 01 and 06 (legacy branch) and 08 (iova2 branch) all assert. If a future
// rdma-core ever stops routing the legacy symbol through iova2, those cases go
// red rather than quietly losing registrations.
DEFINE_PRELOAD_HOOK(ibv, reg_mr_iova2, "ibv_reg_mr_iova2", ibv_mr*, ibv_pd*, void*, size_t,
                    uint64_t, unsigned int);
DEFINE_PRELOAD_HOOK(ibv, dereg_mr, "ibv_dereg_mr", int, ibv_mr*);
DEFINE_PRELOAD_HOOK(ibv, alloc_pd, "ibv_alloc_pd", ibv_pd*, ibv_context*);
DEFINE_PRELOAD_HOOK(ibv, create_qp, "ibv_create_qp", ibv_qp*, ibv_pd*, ibv_qp_init_attr*);
// The conversion an application must call to obtain the new work-request API,
// and therefore the one place the wr_* function pointers can be patched: the
// ibv_wr_* callers are static inline wrappers over pointers inside qp_ex, so
// nothing else can reach them.
//
// Hooked here rather than at QP creation because creation has many doors, and
// every one of them still has to come through this function, which hands us the
// qp_ex directly — so nothing has to guess whether a given ibv_qp is really the
// base of an ibv_qp_ex.
//
// This comment used to claim "perftest reaches the WR API through
// mlx5dv_create_qp, and never calls ibv_create_qp_ex at all (checked with nm on
// the binary)". Both halves were wrong. Entry tracing shows perftest enters
// neither mlx5dv_create_qp nor ibv_create_qp; it creates through
// verbs_context::create_qp_ex, which `nm` cannot see because the caller is a
// static inline. See the create_qp_ex hook above.
DEFINE_PRELOAD_HOOK(ibv, qp_to_qp_ex, "ibv_qp_to_qp_ex", ibv_qp_ex*, ibv_qp*);
DEFINE_PRELOAD_HOOK(ibv, destroy_qp, "ibv_destroy_qp", int, ibv_qp*);
DEFINE_PRELOAD_HOOK(ibv, modify_qp, "ibv_modify_qp", int, ibv_qp*, ibv_qp_attr*, int);
