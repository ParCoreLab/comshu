#pragma once

#include <iostream>

#include "pcomm/helpers/timestamp.hpp"

#include "pcomm/hook/CallbackManager.hpp"
#include "pcomm/hook/HookTrait.hpp"

#include "pcomm/hook/detail/ObjectOpsHookManager.hpp"
#include "pcomm/hook/detail/PreloadHookManager.hpp"

namespace pcomm::hook {
static constexpr bool DebugPrint{false};

// ================================================================
namespace TS = pcomm::helpers::ts;

// Concept requires TimestampSource, other fields have defaults
template<typename T>
concept HookManagerConfig = requires {
    // Must specify a TimestampSource type
    typename T::TimestampSourceType;
    requires TS::TimestampSource<typename T::TimestampSourceType>;
};

struct DefaultHookManagerConfig {
    using TimestampSourceType = TS::RdtscTimestampSource;
};
static_assert(HookManagerConfig<DefaultHookManagerConfig>);
// ================================================================

// ================================================================
// Dealing with Tags
template<typename Tag> struct TaggedBool {
    bool value{false};
};
// ================================================================

// ================================================================
// Concepts for better error messages

// Check if Derived implements get_op
template<typename D, typename Tag, typename Obj>
concept has_get_op_for = requires(D& d, Obj* obj) { d.template get_op<Tag>(obj); };
// ===============================================================

// ================================================================
// CompositeHookManager — one manager for a provider's whole tag set,
// covering both interception mechanisms:
//
//   - preload tags: LD_PRELOAD/dlsym interposers call invoke_preload<Tag>,
//     which lazily resolves the real symbol, fires the registered pre/post
//     callbacks around it, and stamps the call interval.
//   - object-ops tags: install(obj) swaps function pointers inside a
//     vendor object (ibv_context->ops.X) for per-slot trampolines that do
//     the same around the saved original.
//
// Both mechanisms receive every tag and each ignores the other kind (the
// is_preload_hook_v / is_object_ops_hook_v guards); partitioning the packs
// per kind was tried (detail/PartitionTag.hpp, removed — see git history)
// and never paid for its metaprogramming.
//
// Usage:
//   struct MyHooks : CompositeHookManager<MyHooks, DefaultHookManagerConfig,
//                                         tags...> {
//       // Required iff any tag is an object-ops hook: which function
//       // pointer inside the object backs the tag.
//       template<typename Tag>
//       auto& get_op(MyObjectType* obj) { ... }
//   };
// ================================================================
template<typename Derived, HookManagerConfig Config, typename... Tags>
class CompositeHookManager : public CallbackManager<Derived, Tags...> {
    using Base = CallbackManager<Derived, Tags...>;

    using PreloadBase = detail::PreloadHookManager<Tags...>;
    using ObjectOpsBase = detail::ObjectOpsHookManager<Tags...>;

public:
    using Context = detail::ObjectContext<Tags...>;

    using TimestampSource = Config::TimestampSourceType;
    TimestampSource m_timestamp{};

    // Slot-based context storage to avoid map find() in every op
    using ContextSlots = detail::ContextSlots<Context>;

    // Singleton
    static Derived& instance() noexcept {
        static Derived mgr;
        return mgr;
    }

    template<ValidCallbackTag Tag> void internal_register() noexcept {
        if constexpr (is_preload_hook_v<Tag>) {
            static bool is_tag_registered{false};
            if (!is_tag_registered) {
                if constexpr (DebugPrint) {
                    std::cout << "Registering " << HookTrait<Tag>::name << std::endl;
                }
                register_preload_one<Tag>();
            }
            is_tag_registered = true;
        }
    }

    template<ValidCallbackTag Tag> void register_pre(auto&&... args) noexcept {
        std::get<TaggedBool<Tag>>(m_registered_tags).value = true;
        {
            std::scoped_lock lock(m_registration_mutex);
            internal_register<Tag>();
        }
        return Base::template register_pre<Tag>(std::forward<decltype(args)>(args)...);
    }

    template<ValidCallbackTag Tag> void register_post(auto&&... args) noexcept {
        std::get<TaggedBool<Tag>>(m_registered_tags).value = true;
        {
            std::scoped_lock lock(m_registration_mutex);
            internal_register<Tag>();
        }
        return Base::template register_post<Tag>(std::forward<decltype(args)>(args)...);
    }

    // ================================================================
    // 1. Preload hook management
    // ================================================================
    template<typename Tag> void register_preload_one(auto&&... args) noexcept {
        if constexpr (is_preload_hook_v<Tag>) {
            auto default_func = InvokePreloadPtr<Tag, typename HookTrait<Tag>::FuncPtr>::get();
            m_preload_impl.template register_hook<Tag>(std::forward<decltype(args)>(args)...,
                                                       default_func);
        }
    }

    template<ValidCallbackTag Tag>
        requires is_preload_hook_v<Tag>
    [[nodiscard]] HookTrait<Tag>::FuncPtr& real_func() {
        return m_preload_impl.template real_func<Tag>();
    }

    // Static wrapper the extern "C" interposers forward to.
    template<ValidCallbackTag Tag, typename... Args>
        requires is_preload_hook_v<Tag>
    static auto invoke_preload(Args&&... args) noexcept -> HookTrait<Tag>::RetType {
        auto& mgr = instance();

        // Lazy resolution: the real library may not have been loaded at
        // registration time. If we are here at all, the target application
        // just called the symbol, so it is resolvable now — or never.
        auto& real = mgr.template real_func<Tag>();
        if (!real) {
            mgr.m_preload_impl.template register_real<Tag>();
            if (!real) {
                std::cerr << "ERROR: Real function for Tag " << HookTrait<Tag>::name << " is empty!"
                          << std::endl;
                std::abort();
            }
        }

        // No hooks registered for this tag: pure passthrough, no
        // timestamps taken and no callback dispatch.
        if (!std::get<TaggedBool<Tag>>(mgr.m_registered_tags).value) {
            return real(std::forward<Args>(args)...);
        }

        auto ts_start = mgr.m_timestamp.now();

        mgr.template invoke_all_pre<Tag>(args..., ts_start);

        auto ret = real(args...);

        // Skip post hooks if the predicate is false
        if constexpr (HookTrait<Tag>::predicate_fn != nullptr) {
            if (!HookTrait<Tag>::predicate_fn(ret, args...)) {
                return ret;
            }
        }

        // End timestamp
        auto ts_end = mgr.m_timestamp.now();

        mgr.template invoke_all_post<Tag>(ret, std::forward<Args>(args)..., ts_start, ts_end);

        return ret;
    }

    // ================================================================
    // 2. Object-ops hook management
    // ================================================================
    // Swap the object's function pointers for our trampolines. Only tags
    // somebody registered a callback for are patched (install_if_object_ops
    // checks m_registered_tags), so a patched pointer implies a listener —
    // the trampoline never pays for hooks that do not exist.
    template<typename ObjectType> Context* install(ObjectType* obj) noexcept {
        assert(obj && "HookManager ctx obj install called with null obj");

        std::lock_guard lock(m_registration_mutex);

        auto* ctx = m_object_ops_impl.get_context(obj);

        install_object_ops_hooks(obj, ctx, std::make_index_sequence<sizeof...(Tags)>{});

        return ctx;
    }

    // Static trampoline installed into the object's ops table.
    // Note: this takes Args... instead of Args&&... because FuncPtr args need to match exactly.
    // Most of these are small value types or pointers. Should be fine.
    // SlotIndex is a compile-time constant that indexes into ContextSlots::slots
    template<ValidCallbackTag Tag, size_t CtxSlotIndex, typename... Args>
        requires is_object_ops_hook_v<Tag>
    static auto invoke_object_ops(Args... args) noexcept -> typename HookTrait<Tag>::RetType {
        auto& mgr = instance();

        // Start timestamp as soon as the function hook is invoked
        auto ts_start = mgr.m_timestamp.now();

        // Get context from pre-allocated slot
        Context* ctx = ContextSlots::slots[CtxSlotIndex];
        assert(ctx && "ctx is null. This can't happen");

        // Pre-hooks with context
        mgr.template invoke_pre_with_context<Tag>(ctx, args..., ts_start);

        const auto&    original = ctx->template original<Tag>();
        decltype(auto) ret = original(args...);

        // Skip post hooks if the predicate is false
        if constexpr (HookTrait<Tag>::predicate_fn != nullptr) {
            if (!HookTrait<Tag>::predicate_fn(ret, args...)) {
                return ret;
            }
        }

        // End timestamp
        auto ts_end = mgr.m_timestamp.now();

        // Post-hooks with context
        mgr.template invoke_post_with_context<Tag>(ctx, ret, args..., ts_start, ts_end);

        return ret;
    }

protected:
    CompositeHookManager() = default;

    // Expose base class methods
    using Base::invoke_all_post;
    using Base::invoke_all_pre;

    // Context-aware invocation for object-ops
    template<ValidCallbackTag Tag, typename... Args>
    void invoke_pre_with_context(Context* ctx, Args&&... args) noexcept {
        if constexpr (requires(Derived& d, Tag t, Context* c, Args&&... a) {
                          d.pre(t, c, std::forward<Args>(a)...);
                      }) {
            static_cast<Derived&>(*this).pre(Tag{}, ctx, args...);
        }
        Base::template invoke_pre_callbacks<Tag>(std::forward<Args>(args)...);
    }

    template<ValidCallbackTag Tag, typename Ret, typename... Args>
    void invoke_post_with_context(Context* ctx, Ret ret, Args&&... args) noexcept {
        if constexpr (requires(Derived& d, Tag t, Context* c, Ret r, Args&&... a) {
                          d.post(t, c, r, std::forward<Args>(a)...);
                      }) {
            static_cast<Derived&>(*this).post(Tag{}, ctx, ret, args...);
        }
        Base::template invoke_post_callbacks<Tag>(ret, std::forward<Args>(args)...);
    }

private:
    template<typename ObjectType, size_t... Is>
    void install_object_ops_hooks(ObjectType* obj, Context* ctx,
                                  std::index_sequence<Is...>) noexcept {
        (install_if_object_ops<std::tuple_element_t<Is, std::tuple<Tags...>>>(obj, ctx), ...);
    }

    // Alias for the function pointer table
    template<typename Tag, typename FuncPtr>
    using InvokeObjectOpsTable =
        detail::InvokeObjectOpsTable<detail::MAX_OBJECT_OPS_SLOTS, Derived, Tag, FuncPtr>;

    template<ValidCallbackTag Tag, typename ObjectType>
    void install_if_object_ops(ObjectType* obj, Context* ctx) noexcept {
        if constexpr (is_object_ops_hook_v<Tag>) {
            static_assert(has_get_op_for<Derived, Tag, ObjectType>,
                          "Derived must provide: `template <...> get_op<Tag>(ObjectType*)` for "
                          "object-ops hooks");

            if (!std::get<TaggedBool<Tag>>(m_registered_tags).value) {
                if constexpr (DebugPrint) {
                    std::cout << "Skipping " << HookTrait<Tag>::name << std::endl;
                }
                return;
            }

            auto& op = static_cast<Derived&>(*this).template get_op<Tag>(obj);
            ctx->template original<Tag>() = op;

            // Allocate a slot and store context there
            size_t slot = ContextSlots::allocate(ctx);

            // Get the pre-instantiated function pointer for this slot
            using FuncPtr = typename HookTrait<Tag>::FuncPtr;
            op = InvokeObjectOpsTable<Tag, FuncPtr>::table[slot];
        }
    }

    // Adapts invoke_preload<Tag> to the exact function-pointer type the
    // trait declares, so it can be handed around like a plain C pointer.
    template<typename Tag, typename FuncPtr> struct InvokePreloadPtr;

    template<typename Tag, typename Ret, typename... Args>
    struct InvokePreloadPtr<Tag, Ret (*)(Args...) noexcept> {
        static Ret call(Args... args) noexcept {
            return invoke_preload<Tag>(std::forward<Args>(args)...);
        }
        static constexpr auto get() noexcept { return &call; }
    };

    template<typename Tag, typename Ret, typename... Args>
    struct InvokePreloadPtr<Tag, Ret (*)(Args...)> {
        static Ret call(Args... args) { return invoke_preload<Tag>(std::forward<Args>(args)...); }
        static constexpr auto get() { return &call; }
    };

    mutable std::mutex              m_registration_mutex;
    std::tuple<TaggedBool<Tags>...> m_registered_tags;

    // Implementations
    PreloadBase   m_preload_impl;
    ObjectOpsBase m_object_ops_impl;
};

}  // namespace pcomm::hook
