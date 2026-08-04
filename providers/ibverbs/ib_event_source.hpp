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
          Derived, pcomm::hook::DefaultHookManagerConfig, post_send_tag, poll_cq_tag,
          create_qp_ex_tag, open_device_tag, reg_mr_iova2_tag, dereg_mr_tag, alloc_pd_tag,
          create_qp_tag, qp_to_qp_ex_tag, destroy_qp_tag, modify_qp_tag> {
    using Base = pcomm::hook::CompositeHookManager<
        Derived, pcomm::hook::DefaultHookManagerConfig, post_send_tag, poll_cq_tag,
        create_qp_ex_tag, open_device_tag, reg_mr_iova2_tag, dereg_mr_tag, alloc_pd_tag,
        create_qp_tag, qp_to_qp_ex_tag, destroy_qp_tag, modify_qp_tag>;

public:
    using Context = typename Base::Context;

    static void* GetLibHandle() noexcept { return nullptr; }

    // Which function-pointer slot backs each object-ops tag. post_send and
    // poll_cq live in ibv_context->ops; create_qp_ex lives one struct out, in
    // the verbs_context that ibv_context is embedded in.
    template<pcomm::hook::ValidCallbackTag Tag> auto& get_op(ibv_context* ctx) const noexcept {
        if constexpr (std::is_same_v<Tag, post_send_tag>) {
            return ctx->ops.post_send;
        } else if constexpr (std::is_same_v<Tag, create_qp_ex_tag>) {
            return create_qp_ex_slot(ctx);
        } else {  // poll_cq_tag
            return ctx->ops.poll_cq;
        }
    }

protected:
    IbverbsEventSource() = default;

private:
    using CreateQpExFn = ibv_qp* (*)(ibv_context*, ibv_qp_init_attr_ex*);

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
    CreateQpExFn& create_qp_ex_slot(ibv_context* ctx) const noexcept {
        verbs_context* vctx = verbs_get_ctx(ctx);
        const bool     usable = vctx != nullptr &&
                            vctx->sz >= sizeof(*vctx) - offsetof(verbs_context, create_qp_ex) &&
                            vctx->create_qp_ex != nullptr;

        if (!usable) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                             "[pcomm] ibverbs: no usable verbs_context::create_qp_ex on this "
                             "device; extended queue-pair creation will not be recorded "
                             "(ibv_create_qp is still hooked)\n");
            }
            return m_create_qp_ex_sink;
        }
        return vctx->create_qp_ex;
    }

    mutable CreateQpExFn m_create_qp_ex_sink{nullptr};
};

// The provider singleton; template instantiated once in ibverbs_provider.cpp.
struct Profiler : IbverbsEventSource<Profiler> {
    Profiler();
};
extern template class IbverbsEventSource<Profiler>;

}  // namespace ibv
