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
// The other half of the wire, and until now the half pcomm never saw. Same
// shape as post_send — a linked list of fully-formed work requests, so one
// hooked call sees the whole chain — and the same ibv_context->ops slot family.
//
// There is no receive counterpart to the ibv_wr_* builder API that made sends
// invisible on perftest: every ibv_wr_* entry point is a SEND opcode, so a
// receive has exactly one door and this is it. That is why post_recv needs no
// equivalent of the qp_to_qp_ex pointer-patching that post_send required.
DEFINE_OBJECT_OPS_HOOK(ibv, post_recv, "ibv_post_recv", int, ibv_qp*, ibv_recv_wr*, ibv_recv_wr**);
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
// Extended completion-queue creation, and an object-ops hook for exactly the
// reason create_qp_ex above is one — checked, not assumed:
//
//   $ readelf --dyn-syms libibverbs.so.1 | grep -o 'ibv_create_cq[a-z_]*'
//     ibv_create_cq
//
// ibv_create_cq_ex is a static inline over verbs_context::create_cq_ex
// (verbs.h), so the exported ibv_create_cq is NOT the only door to a CQ, and
// hooking only it would have left ib_completion_queue with the same hole
// ib_queue_pair had in case 08 — an entity referenced by events that nothing
// created. Hooking both doors costs one vctx_slot line and removes the need to
// be right about which one an application compiled against.
//
// Returns ibv_cq_ex*, whose layout starts with ibv_cq's exactly — verbs.h makes
// ibv_cq_ex_to_cq a plain cast — so the same pointer interns to the same Handle
// whichever door created it, and IbPollCq.cq joins either way.
DEFINE_OBJECT_OPS_HOOK(ibv, create_cq_ex, "ibv_create_cq_ex", ibv_cq_ex*, ibv_context*,
                       ibv_cq_init_attr_ex*);
// The shared-receive-queue receive door. An object-ops hook whose first argument
// is an ibv_srq* rather than an ibv_qp* — which matters for SigmaTrace, whose
// get_object_from_args must resolve the context from that argument
// (IbverbsEventSource.hpp:97), and does NOT matter here: pcomm's object-ops
// trampolines carry their context in a per-context slot allocated at install
// time, so the argument types are never inspected to find it.
DEFINE_OBJECT_OPS_HOOK(ibv, post_srq_recv, "ibv_post_srq_recv", int, ibv_srq*, ibv_recv_wr*,
                       ibv_recv_wr**);
// Extended SRQ creation, and the door that actually justifies the "an SRQ can
// exist before any PD" argument: ibv_create_srq takes a *pd* and therefore
// cannot precede one, while ibv_create_srq_ex takes a *context* and can. Like
// create_cq_ex and create_qp_ex it is a verbs_context slot with no exported
// symbol, so this is its only door.
DEFINE_OBJECT_OPS_HOOK(ibv, create_srq_ex, "ibv_create_srq_ex", ibv_srq*, ibv_context*,
                       ibv_srq_init_attr_ex*);
// Arming a CQ for a completion notification. An ibv_context->ops slot like
// poll_cq, and the counterpart to it: together they say whether an application
// is spinning or sleeping.
DEFINE_OBJECT_OPS_HOOK(ibv, req_notify_cq, "ibv_req_notify_cq", int, ibv_cq*, int);
DEFINE_OBJECT_OPS_HOOK_PRED(
    ibv, poll_cq, "ibv_poll_cq",
    [](int out_ret, ibv_cq*, int, ibv_wc*) {
        // Skip the empty-poll firehose; record only polls that reaped completions.
        return out_ret > 0;
    },
    int, ibv_cq*, int, ibv_wc*);

// --- preload hooks (LD_PRELOAD / dlsym interposition) ------------------------
DEFINE_PRELOAD_HOOK(ibv, open_device, "ibv_open_device", ibv_context*, ibv_device*);
// The other end of a context's life, and the reason any set keyed on
// ibv_context* needs one. Without it a closed context stays "known" forever,
// and since the allocator reuses the address, the NEXT context at that address
// is treated as already handled. That silently cost case 02 two of its eight
// ranks (26880 sends -> 20160) until it was bisected against 055d18b.
DEFINE_PRELOAD_HOOK(ibv, close_device, "ibv_close_device", int, ibv_context*);
// libmlx5's context-open door, and the earliest install point for the contexts
// that were arriving late. UCX's mlx5 transport (libuct_ib_mlx5.so) opens a
// DEVX-capable context here rather than through ibv_open_device — the same
// object entity_close_accounted found destroying CQs nothing created, because a
// context first seen at ibv_create_cq ran uninstrumented through every
// create_cq_ex (an object-ops slot) it issued before that. Hooking the open
// gives those contexts a zero-length blind window, like ibv_open_device.
//
// An exported symbol (checked with readelf), unlike its create_qp_ex cousin, so
// a plain preload hook reaches it. Third argument is mlx5dv_context_attr* in
// mlx5dv.h and void* here, for the same reason mlx5dv_create_cq's is: keep the
// mlx5dv.h dependency off the one provider that must build against only
// libibverbs. It is passed through untouched and never dereferenced.
DEFINE_PRELOAD_HOOK(ibv, mlx5dv_open_device, "mlx5dv_open_device", ibv_context*, ibv_device*,
                    void*);
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
DEFINE_OBJECT_OPS_HOOK(ibv, reg_dm_mr, "ibv_reg_dm_mr", ibv_mr*, ibv_pd*, ibv_dm*, uint64_t, size_t,
                       unsigned int);
DEFINE_OBJECT_OPS_HOOK(ibv, alloc_null_mr, "ibv_alloc_null_mr", ibv_mr*, ibv_pd*);
DEFINE_PRELOAD_HOOK(ibv, dereg_mr, "ibv_dereg_mr", int, ibv_mr*);
DEFINE_PRELOAD_HOOK(ibv, alloc_pd, "ibv_alloc_pd", ibv_pd*, ibv_context*);
// Hooked for COVERAGE first, and now for a record as well.
//
// Object-ops hooks live inside an ibv_context, and a context is instrumented at
// its first call through a hooked door. Job 44396249 measured which door that
// is: 48 of 73 contexts per rank arrive first at ibv_alloc_pd, i.e. they were
// opened somewhere we do not interpose, so a window exists between their open
// and that alloc_pd. Every object-op we hook needs a PD to exist first — a send
// needs a QP, a QP needs a PD, reg_dm_mr and alloc_null_mr take one — with the
// single exception of ibv_poll_cq, whose CQ is created from a CONTEXT and not
// from a PD. This is that exception's door, and interposing it is what turns
// "ordering says nothing can be lost" into something measured: if a context
// ever installs at door=create_cq, a CQ existed on it before any PD did.
// ...and, as of this change, for a record too: ib_completion_queue. The
// coverage argument above is why the hook was here first, but a door held open
// for a record nobody wrote is the failure mode this provider has spent a week
// paying down — IbPollCq.cq had no entity to join to for as long as ib_poll_cq
// has existed.
DEFINE_PRELOAD_HOOK(ibv, create_cq, "ibv_create_cq", ibv_cq*, ibv_context*, int, void*,
                    ibv_comp_channel*, int);
// The CQ's other end. Same argument as close_device and dereg_mr: without it a
// destroyed CQ stays "live" forever in the capture, and since the allocator
// reuses the address, the next CQ at that address is indistinguishable from it.
// It is also what puts CQs under suite/asserts/entity_close_accounted.sql,
// which is the assertion that has found every missing door so far.
DEFINE_PRELOAD_HOOK(ibv, destroy_cq, "ibv_destroy_cq", int, ibv_cq*);
// The THIRD completion-queue door, and it is not in libibverbs at all.
//
// IbCqOrigin was added as an instrument with a written-down prediction: "a
// deficit that is entirely CreateCq rules the install window out; one that is
// entirely CreateCqEx implicates it; neither means a third door." The
// instrument then answered. On 16_hubspoke_ib (job 44459283) every rank records
// 104 create_cq + 60 create_cq_ex opens and 194 closes — a deficit of exactly
// 30, on all four ranks, split 15 closes naming a handle nothing opened and 15
// naming a handle opened once and closed twice. Both halves are the same fact:
// an address the allocator reused lands on a handle we already interned (a
// second close), a fresh address does not (an orphan). Not a double-emit.
//
// The third door is mlx5dv_create_cq, and unlike ibv_create_cq_ex it IS an
// exported symbol — `mlx5dv_create_cq@@MLX5_1.1` in libmlx5.so.1 — so a plain
// preload hook reaches it. It is the only undefined create_cq-shaped reference
// in the whole stack besides ibv_create_cq: checked with readelf across every
// .so under UCX, OpenMPI and NVSHMEM on MN5, and libuct_ib_mlx5.so.0 is the one
// object that names it. UCX's mlx5 transport takes that path for its tx and rx
// CQs and destroys them through ibv_destroy_cq, which is why the close arrived
// and the open never did.
//
// rdma-core implements it over libmlx5's INTERNAL create_cq rather than over
// the verbs_context::create_cq_ex slot, so hooking it does NOT double-count the
// way ibv_reg_mr over ibv_reg_mr_iova2 did — and that is measured rather than
// read off the source: if it routed through the slot, those 30 CQs would
// already be in the capture with origin=create_cq_ex.
//
// WHY IT LIVES IN THIS PROVIDER, next to a libibverbs symbol. Because
// ibv_destroy_cq does, and an entity whose open and close are emitted by
// different providers is an entity whose accounting depends on which providers
// the user selected: `pcomm -p ib` alone would record every close and no open,
// and entity_close_accounted would go red on a correct capture. mlx5dv_create_qp
// took the other choice and left exactly that hole — mlx5_queue_pair is an open
// record in the mlx5dv range while ibv_destroy_qp's close comes from here, so
// the assertion has to be taught about the table by hand.
//
// The third argument is mlx5dv_cq_init_attr*, declared void* here on purpose:
// it is passed through untouched, a pointer is a pointer, and typing it would
// put an mlx5dv.h dependency on the one provider that must build against
// nothing but libibverbs.
DEFINE_PRELOAD_HOOK(ibv, mlx5dv_create_cq, "mlx5dv_create_cq", ibv_cq_ex*, ibv_context*,
                    ibv_cq_init_attr_ex*, void*);
// The classic SRQ doors. Both ARE exported symbols (checked with readelf, unlike
// their _ex cousin above), so a plain preload hook reaches them.
// The UD addressing doors. Both exported (checked with readelf).
DEFINE_PRELOAD_HOOK(ibv, create_ah, "ibv_create_ah", ibv_ah*, ibv_pd*, ibv_ah_attr*);
DEFINE_PRELOAD_HOOK(ibv, destroy_ah, "ibv_destroy_ah", int, ibv_ah*);
// Hooked as an INSTALL DOOR only, and deliberately without a callback — which
// is the one case this provider allows it, the same standing create_cq had
// before it earned a record. ibv_query_device takes a context, so it can be a
// context's first call through anything we interpose.
//
// The ib_device record it might have carried is emitted from ensure_installed
// instead, because a row that exists only when the application chooses to ask
// is a row that is usually absent. See IbDevice in ib_records.hpp.
//
// ibv_get_device_guid is NOT hooked, though SigmaTrace hooks it. It takes an
// ibv_device* rather than a context, so it is useless as an install door, and
// the guid it returns is already node_guid in the device_attr ib_device
// carries — so the hook would cost a dlsym chain and add nothing. That is the
// same test the five silent SigmaTrace doors failed.
DEFINE_PRELOAD_HOOK(ibv, query_device, "ibv_query_device", int, ibv_context*, ibv_device_attr*);
DEFINE_PRELOAD_HOOK(ibv, create_srq, "ibv_create_srq", ibv_srq*, ibv_pd*, ibv_srq_init_attr*);
DEFINE_PRELOAD_HOOK(ibv, destroy_srq, "ibv_destroy_srq", int, ibv_srq*);
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
// ibv_destroy_qp is NOT hooked here. Its EntityClose is emitted by the ENGINE
// (libpcomm_engine.so), because a QP opened via mlx5dv_create_qp (mlx5dv
// provider) is destroyed through this same door, and only the engine is present
// in every provider selection. See ibv_destroy_qp in src/engine.cpp.
DEFINE_PRELOAD_HOOK(ibv, modify_qp, "ibv_modify_qp", int, ibv_qp*, ibv_qp_attr*, int);
