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
// That routing is CONDITIONAL, and reading it as universal cost this project a
// long hunt. MN5 job 44368331 traced the CALLER of every registration door:
// UCX's mlx5 transport (libuct_ib_mlx5.so.0) called ibv_dereg_mr and never any
// hooked registration door, and it references ibv_reg_mr@IBVERBS_1.1 — the
// legacy symbol. No libibverbs.so.1 appeared as a caller of iova2 in that run,
// so in that process the legacy symbol did NOT route through it. Those closes
// were the 33 unmatched rows entity_close_accounted had been reporting.
//
// So both are hooked, and the double-count is prevented directly instead of by
// declining to hook: a thread-local depth guard (kRegMrDepth in the provider)
// makes the LEGACY hook the recorder and suppresses the nested iova2 callback
// when the routing does happen. Nesting is a fact observable at run time; which
// rdma-core build you are on is not.
DEFINE_PRELOAD_HOOK(ibv, reg_mr_iova2, "ibv_reg_mr_iova2", ibv_mr*, ibv_pd*, void*, size_t,
                    uint64_t, unsigned int);
// The legacy registration symbol. Four arguments, not six: no iova (it is
// implicitly addr) and access is a plain int.
DEFINE_PRELOAD_HOOK(ibv, reg_mr, "ibv_reg_mr", ibv_mr*, ibv_pd*, void*, size_t, int);
// The dmabuf registration door, and it was invisible until entity_close_accounted
// found it: closes naming regions nothing created, ~47 of 290 per rank on case
// 02 (MN5 44358674). NCCL calls it (nccl src/transport/net_ib/reg.cc:35) and so
// does NVSHMEM (nvshmem transport_ib_common.cpp:921), while ib_pingpong and
// perftest do not — which is exactly which cases failed.
//
// A separate symbol from reg_mr/reg_mr_iova2, not a wrapper over them, so
// hooking those never saw it. Unlike ibv_create_qp_ex this IS exported
// (verbs.h:2632), so a plain preload hook reaches it.
DEFINE_PRELOAD_HOOK(ibv, reg_dmabuf_mr, "ibv_reg_dmabuf_mr", ibv_mr*, ibv_pd*, uint64_t, size_t,
                    uint64_t, int, int);
// The two MEMORYLESS registration doors, and neither is a symbol. Both are
// `static inline` over verbs_context function pointers (verbs.h:2807, 2830), so
// no ELF reference names them, no dlsym resolves them, and every instrument that
// answers questions about symbols reports nothing — the ibv_create_qp_ex shape
// exactly, which cost case 08 a week.
//
// Found by following the mr rather than the symbol (MN5 job 44368870): 474
// closes per run of regions nothing opened, all addr=NULL, in two populations —
// 355 at 32 bytes from uct_ib_dereg_mr, 120 at 2048 from
// uct_rc_mlx5_iface_common_dm_cleanup. DM is the NIC's own memory, so those
// regions have no host address and `range.base` is 0 truthfully rather than for
// want of resolving it. IbMemRegion.origin is what says which.
DEFINE_OBJECT_OPS_HOOK(ibv, reg_dm_mr, "ibv_reg_dm_mr", ibv_mr*, ibv_pd*, ibv_dm*, uint64_t,
                       size_t, unsigned int);
DEFINE_OBJECT_OPS_HOOK(ibv, alloc_null_mr, "ibv_alloc_null_mr", ibv_mr*, ibv_pd*);
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
