#pragma once
// IbverbsEventSource — binds the ibverbs hook tags to pcomm's interception
// engine. Ported from SigmaTrace/Ibtrace/IbverbsEventSource.hpp: the object-ops
// contract (get_op) tells CompositeHookManager which ibv_context->ops slot
// backs each object-ops tag; everything else is the tag list.

#include <cstddef>
#include <cstdio>
#include <string_view>
#include <type_traits>

#include "pcomm/hook/CompositeHookManager.hpp"

#include "ib_hooks.hpp"

namespace ibv {
constexpr std::string_view LibPath{"libibverbs.so"};

template<typename Derived>
class IbverbsEventSource
    : public pcomm::hook::CompositeHookManager<
          Derived, pcomm::hook::DefaultHookManagerConfig, post_send_tag, post_recv_tag, post_srq_recv_tag, req_notify_cq_tag, poll_cq_tag,
          create_qp_ex_tag, create_cq_ex_tag, create_srq_ex_tag, reg_dm_mr_tag, alloc_null_mr_tag, open_device_tag, close_device_tag, reg_mr_iova2_tag, reg_mr_tag, reg_dmabuf_mr_tag, dereg_mr_tag, alloc_pd_tag, create_cq_tag, destroy_cq_tag, mlx5dv_create_cq_tag, mlx5dv_open_device_tag, create_srq_tag, destroy_srq_tag, create_ah_tag, destroy_ah_tag, query_device_tag,
          create_qp_tag, qp_to_qp_ex_tag, destroy_qp_tag, modify_qp_tag> {
    using Base = pcomm::hook::CompositeHookManager<
        Derived, pcomm::hook::DefaultHookManagerConfig, post_send_tag, post_recv_tag, post_srq_recv_tag, req_notify_cq_tag, poll_cq_tag,
        create_qp_ex_tag, create_cq_ex_tag, create_srq_ex_tag, reg_dm_mr_tag, alloc_null_mr_tag, open_device_tag, close_device_tag, reg_mr_iova2_tag, reg_mr_tag, reg_dmabuf_mr_tag, dereg_mr_tag, alloc_pd_tag, create_cq_tag, destroy_cq_tag, mlx5dv_create_cq_tag, mlx5dv_open_device_tag, create_srq_tag, destroy_srq_tag, create_ah_tag, destroy_ah_tag, query_device_tag,
        create_qp_tag, qp_to_qp_ex_tag, destroy_qp_tag, modify_qp_tag>;

public:
    using Context = typename Base::Context;

    static void* GetLibHandle() noexcept { return nullptr; }

    // Which function-pointer slot backs each object-ops tag. post_send,
    // post_recv and poll_cq live in ibv_context->ops; the *_ex slots live one
    // struct out, in the verbs_context that ibv_context is embedded in, and go
    // through vctx_slot for the two guards that door needs.
    template<pcomm::hook::ValidCallbackTag Tag> auto& get_op(ibv_context* ctx) const noexcept {
        if constexpr (std::is_same_v<Tag, post_send_tag>) {
            return ctx->ops.post_send;
        } else if constexpr (std::is_same_v<Tag, post_recv_tag>) {
            return ctx->ops.post_recv;
        } else if constexpr (std::is_same_v<Tag, post_srq_recv_tag>) {
            return ctx->ops.post_srq_recv;
        } else if constexpr (std::is_same_v<Tag, req_notify_cq_tag>) {
            return ctx->ops.req_notify_cq;
        } else if constexpr (std::is_same_v<Tag, create_qp_ex_tag>) {
            return vctx_slot(ctx, &verbs_context::create_qp_ex,
                             offsetof(verbs_context, create_qp_ex), "create_qp_ex",
                             m_create_qp_ex_sink, m_create_qp_ex_warned);
        } else if constexpr (std::is_same_v<Tag, create_cq_ex_tag>) {
            return vctx_slot(ctx, &verbs_context::create_cq_ex,
                             offsetof(verbs_context, create_cq_ex), "create_cq_ex",
                             m_create_cq_ex_sink, m_create_cq_ex_warned);
        } else if constexpr (std::is_same_v<Tag, create_srq_ex_tag>) {
            return vctx_slot(ctx, &verbs_context::create_srq_ex,
                             offsetof(verbs_context, create_srq_ex), "create_srq_ex",
                             m_create_srq_ex_sink, m_create_srq_ex_warned);
        } else if constexpr (std::is_same_v<Tag, reg_dm_mr_tag>) {
            return vctx_slot(ctx, &verbs_context::reg_dm_mr,
                             offsetof(verbs_context, reg_dm_mr), "reg_dm_mr", m_reg_dm_mr_sink,
                             m_reg_dm_mr_warned);
        } else if constexpr (std::is_same_v<Tag, alloc_null_mr_tag>) {
            return vctx_slot(ctx, &verbs_context::alloc_null_mr,
                             offsetof(verbs_context, alloc_null_mr), "alloc_null_mr",
                             m_alloc_null_mr_sink, m_alloc_null_mr_warned);
        } else {  // poll_cq_tag
            return ctx->ops.poll_cq;
        }
    }

protected:
    IbverbsEventSource() = default;

private:
    using CreateQpExFn = ibv_qp* (*)(ibv_context*, ibv_qp_init_attr_ex*);
    using CreateCqExFn = ibv_cq_ex* (*)(ibv_context*, ibv_cq_init_attr_ex*);
    using CreateSrqExFn = ibv_srq* (*)(ibv_context*, ibv_srq_init_attr_ex*);
    using RegDmMrFn = ibv_mr* (*)(ibv_pd*, ibv_dm*, uint64_t, size_t, unsigned int);
    using AllocNullMrFn = ibv_mr* (*)(ibv_pd*);

    // Reproduces the two guards verbs_get_ctx_op applies before calling this
    // slot (verbs.h:2230), because patching it when either fails is worse than
    // not capturing:
    //
    //  - `sz` — verbs_context "grows up", so a libibverbs older than the field
    //    simply has no create_qp_ex and writing there lands past the struct.
    //
    //  - a NULL slot means the driver does not implement extended creation, and
    //    ibv_create_qp_ex falls back to ibv_create_qp — which we already hook.
    //    Patching a NULL slot would make verbs_get_ctx_op report the capability
    //    as present, sending the application down a path it had opted out of and
    //    into our trampoline, whose saved original is NULL. That is the
    //    devx_probe mistake exactly: a hook must never manufacture a capability
    //    or a failure the application did not have.
    //
    // When either guard fails the tag is pointed at an inert sink, so install()
    // patches a slot nothing dispatches through, and the reason is printed once
    // rather than left as an empty table.
    // Generalised from create_qp_ex_slot when reg_dm_mr and alloc_null_mr turned
    // out to be the same shape of door. Every verbs_context slot needs the same
    // two guards, and getting either wrong is worse than not capturing.
    template<typename Fn>
    Fn& vctx_slot(ibv_context* ctx, Fn verbs_context::*slot, std::size_t slot_offset,
                  const char* what, Fn& sink, bool& warned) const noexcept {
        verbs_context* vctx = verbs_get_ctx(ctx);
        const bool     usable = vctx != nullptr && vctx->sz >= sizeof(*vctx) - slot_offset &&
                            (vctx->*slot) != nullptr;
        if (!usable) {
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                             "[pcomm] ibverbs: no usable verbs_context::%s on this device; "
                             "calls through it will not be recorded\n",
                             what);
            }
            return sink;
        }
        return vctx->*slot;
    }

    mutable CreateQpExFn   m_create_qp_ex_sink{nullptr};
    mutable CreateCqExFn   m_create_cq_ex_sink{nullptr};
    mutable CreateSrqExFn  m_create_srq_ex_sink{nullptr};
    mutable RegDmMrFn      m_reg_dm_mr_sink{nullptr};
    mutable AllocNullMrFn  m_alloc_null_mr_sink{nullptr};
    mutable bool           m_create_qp_ex_warned{false};
    mutable bool           m_create_cq_ex_warned{false};
    mutable bool           m_create_srq_ex_warned{false};
    mutable bool           m_reg_dm_mr_warned{false};
    mutable bool           m_alloc_null_mr_warned{false};
};

// The provider singleton; template instantiated once in ibverbs_provider.cpp.
struct Profiler : IbverbsEventSource<Profiler> {
    Profiler();
};
extern template class IbverbsEventSource<Profiler>;

}  // namespace ibv
