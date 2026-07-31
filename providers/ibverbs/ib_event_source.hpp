#pragma once
// IbverbsEventSource — binds the ibverbs hook tags to pcomm's interception
// engine. Ported from SigmaTrace/Ibtrace/IbverbsEventSource.hpp: the object-ops
// contract (get_op / get_object_from_args) tells CompositeHookManager how to
// reach ibv_context->ops.X and how to recover the context from a call's args.

#include <string_view>
#include <tuple>
#include <type_traits>

#include "pcomm/hook/CompositeHookManager.hpp"

#include "ib_hooks.hpp"

namespace ibv {
    constexpr std::string_view LibPath{"libibverbs.so"};

    template<typename Derived>
    class IbverbsEventSource
        : public pcomm::hook::CompositeHookManager<
              Derived, pcomm::hook::DefaultHookManagerConfig, ibv_context,
              post_send_tag, poll_cq_tag,
              open_device_tag, reg_mr_tag, dereg_mr_tag, alloc_pd_tag,
              create_qp_tag, qp_to_qp_ex_tag, destroy_qp_tag, modify_qp_tag> {

        using Base = pcomm::hook::CompositeHookManager<
              Derived, pcomm::hook::DefaultHookManagerConfig, ibv_context,
              post_send_tag, poll_cq_tag,
              open_device_tag, reg_mr_tag, dereg_mr_tag, alloc_pd_tag,
              create_qp_tag, qp_to_qp_ex_tag, destroy_qp_tag, modify_qp_tag>;

    public:
        using Context = typename Base::Context;

        static void* GetLibHandle() noexcept { return nullptr; }

        // Which ibv_context->ops slot backs each object-ops tag.
        template<pcomm::hook::ValidCallbackTag Tag>
        auto& get_op(ibv_context* ctx) const noexcept {
            if constexpr (std::is_same_v<Tag, post_send_tag>) {
                return ctx->ops.post_send;
            } else {  // poll_cq_tag
                return ctx->ops.poll_cq;
            }
        }

        // Recover the ibv_context from an object-ops call's first argument.
        template<pcomm::hook::ValidCallbackTag Tag, typename... Args>
        ibv_context* get_object_from_args(Args&&... args) noexcept {
            auto first_arg = std::get<0>(std::make_tuple(args...));
            if constexpr (std::is_same_v<Tag, post_send_tag>) {
                return reinterpret_cast<ibv_qp*>(first_arg)->context;
            } else if constexpr (std::is_same_v<Tag, poll_cq_tag>) {
                return reinterpret_cast<ibv_cq*>(first_arg)->context;
            } else {
                return nullptr;
            }
        }

    protected:
        IbverbsEventSource() = default;
    };

    // The provider singleton; template instantiated once in ibverbs_provider.cpp.
    struct Profiler : IbverbsEventSource<Profiler> {
        Profiler();
    };
    extern template class IbverbsEventSource<Profiler>;

}  // namespace ibv
