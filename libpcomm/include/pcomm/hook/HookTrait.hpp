#pragma once
// HookTrait — the compile-time description of one intercepted function.
//
// A hook is identified by an empty tag struct; HookTrait<tag> carries its
// symbol name, signature, install mechanism (HookType) and optional post-hook
// predicate. Providers declare hooks with the DEFINE_*_HOOK macros below
// (see providers/ibverbs/ib_hooks.hpp), which must be invoked at global scope:
// each expands to a tag struct in the given namespace plus a specialization of
// ::pcomm::hook::HookTrait.

#include <concepts>
#include <type_traits>

#include "pcomm/helpers/timestamp.hpp"

namespace pcomm::hook {
using pcomm::helpers::ts::Timestamp;

// ================================================================
// Hook type - how is this hook installed?
// ================================================================
enum class HookType {
    ObjectOps,  // Replace function pointer in object (ctx->ops.X)
    Preload,    // LD_PRELOAD / dlsym interception
};

// Specialized per tag by the DEFINE_*_HOOK macros. Every specialization
// provides: FuncPtr, RetType, PreCallback, PostCallback, predicate_fn,
// type, name, symbol_name.
template<typename Tag> struct HookTrait;

template<typename Tag, typename = void> struct has_hook_trait : std::false_type {};

template<typename Tag>
struct has_hook_trait<Tag, std::void_t<typename HookTrait<Tag>::FuncPtr>> : std::true_type {};

template<typename Tag> inline constexpr bool has_hook_trait_v = has_hook_trait<Tag>::value;

// A tag with a complete HookTrait specialization.
template<typename Tag>
concept ValidCallbackTag = requires {
    { HookTrait<Tag>::type } -> std::convertible_to<HookType>;
    { HookTrait<Tag>::name } -> std::convertible_to<const char*>;
    typename HookTrait<Tag>::FuncPtr;
    typename HookTrait<Tag>::RetType;
    typename HookTrait<Tag>::PreCallback;
    typename HookTrait<Tag>::PostCallback;
};

// ================================================================
// Hook definition macros.
//
//   DEFINE_PRELOAD_HOOK(ns, name, "symbol", ret, args...)
//   DEFINE_OBJECT_OPS_HOOK(ns, name, "symbol", ret, args...)
//   DEFINE_OBJECT_OPS_HOOK_PRED(ns, name, "symbol", pred, ret, args...)
//
// `pred` is a captureless lambda bool(ret, args...); when it returns false
// the post callbacks are skipped (see poll_cq: don't record empty polls).
//
// Preload FuncPtrs are noexcept (our interposers are); object-ops pointers
// come from a C ABI struct and cannot be. That one token is the only
// difference between the expansions, so it is a macro argument.
// ================================================================
// Callback signatures, with the void-returning case spelled separately.
//
// A hook whose function returns void cannot have `ret_type` in its callback
// signatures: `bool (*)(void, Args...)` is ill-formed, and so is
// `void (*)(void, Args...)`. Without this the macro simply fails to compile for
// such a symbol, which is how mlx5dv_devx_free_uar ended up unhooked and a UAR
// lifetime ended up with no close. SigmaTrace passes std::monostate as the
// return type to dodge the same wall; spelling the specialisation is cheaper
// than teaching every provider a placeholder convention.
//
// For a void hook the post callback simply has no result parameter — there is
// no result. Everything else is identical.
namespace detail {  // already inside pcomm::hook — see the opener above

template<typename Ret, typename... Args> struct HookSigs {
    using RunPredicateFn = bool (*)(Ret, Args...);
    using PreCallback = void (*)(Args..., const Timestamp);
    using PostCallback = void (*)(Ret, Args..., const Timestamp, const Timestamp);
};

template<typename... Args> struct HookSigs<void, Args...> {
    using RunPredicateFn = bool (*)(Args...);
    using PreCallback = void (*)(Args..., const Timestamp);
    using PostCallback = void (*)(Args..., const Timestamp, const Timestamp);
};

}  // namespace detail

#define PCOMM_DEFINE_HOOK_(ns, hook_name, sym_name, hook_type, pred, noexcept_spec, ret_type, ...) \
    namespace ns {                                                                                 \
    struct hook_name##_tag {};                                                                     \
    }                                                                                              \
    template<> struct pcomm::hook::HookTrait<ns::hook_name##_tag> {                                \
        using FuncPtr = ret_type (*)(__VA_ARGS__) noexcept_spec;                                   \
        using RetType = ret_type;                                                                  \
        using Sigs = ::pcomm::hook::detail::HookSigs<ret_type, __VA_ARGS__>;                         \
        using RunPredicateFn = typename Sigs::RunPredicateFn;                                      \
        static constexpr RunPredicateFn predicate_fn = pred;                                       \
        using PreCallback = typename Sigs::PreCallback;                                            \
        using PostCallback = typename Sigs::PostCallback;                                          \
        static constexpr pcomm::hook::HookType type = pcomm::hook::HookType::hook_type;            \
        static constexpr const char*           name = #hook_name;                                  \
        static constexpr const char*           symbol_name = sym_name;                             \
    }

#define DEFINE_PRELOAD_HOOK(ns, hook_name, sym_name, ret_type, ...)                                \
    PCOMM_DEFINE_HOOK_(ns, hook_name, sym_name, Preload, nullptr, noexcept, ret_type, __VA_ARGS__)

#define DEFINE_PRELOAD_HOOK_PRED(ns, hook_name, sym_name, pred, ret_type, ...)                     \
    PCOMM_DEFINE_HOOK_(ns, hook_name, sym_name, Preload, pred, noexcept, ret_type, __VA_ARGS__)

#define DEFINE_OBJECT_OPS_HOOK(ns, hook_name, sym_name, ret_type, ...)                             \
    PCOMM_DEFINE_HOOK_(ns, hook_name, sym_name, ObjectOps, nullptr, , ret_type, __VA_ARGS__)

#define DEFINE_OBJECT_OPS_HOOK_PRED(ns, hook_name, sym_name, pred, ret_type, ...)                  \
    PCOMM_DEFINE_HOOK_(ns, hook_name, sym_name, ObjectOps, pred, , ret_type, __VA_ARGS__)

// ================================================================
// Type traits for hook categorization
// ================================================================
template<typename Tag>
struct is_preload_hook : std::bool_constant<HookTrait<Tag>::type == HookType::Preload> {};

template<typename Tag> inline constexpr bool is_preload_hook_v = is_preload_hook<Tag>::value;

template<typename Tag>
struct is_object_ops_hook : std::bool_constant<HookTrait<Tag>::type == HookType::ObjectOps> {};

template<typename Tag> inline constexpr bool is_object_ops_hook_v = is_object_ops_hook<Tag>::value;

}  // namespace pcomm::hook
