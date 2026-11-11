#pragma once

#include <cassert>

#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <unordered_map>

#include "comshu/hook/HookTrait.hpp"

#include "comshu/hook/detail/TagIndex.hpp"

namespace comshu::hook::detail {
// ================================================================
// Maximum number of object-ops slots. One slot is consumed per (object-ops TAG,
// context), not per context — so with N tags this holds MAX/N contexts.
//
// 256 was chosen when the comment below read "hopefully no one has 257 NICs in
// one node", i.e. assuming one context per NIC. That assumption is wrong on real
// applications: MN5 job 44369505 saw 159 distinct ibv_contexts across 8 ranks —
// ~20 per rank — because UCX opens a context per device per worker. With five
// object-ops tags that is ~100 slots against a ceiling of 256, the same order of
// magnitude as the limit rather than comfortably below it.
// 1024, not more: InvokeObjectOpsTable instantiates ONE trampoline per slot per
// tag (the slot index is a template parameter, since the trampoline has no
// argument to carry it), so the ceiling is paid in generated code — 5 tags x
// 1024 = ~5k functions, against ~1.3k before. 4096 was tried first and rejected
// for that reason, not because it was insufficient.
//
// 1024 is ~10x the ~100 slots that job measured, and the overflow path below is
// now survivable, so being wrong here costs capture rather than the run.
static constexpr size_t MAX_OBJECT_OPS_SLOTS = 1024;

// Returned when the table is full. Instrumentation stops; the application does
// not.
static constexpr size_t kNoObjectOpsSlot = static_cast<size_t>(-1);

// ================================================================
// Slot-based storage for context pointers to avoid find() for each op
// Each Tag gets its own set of slots to store Context pointers
// This is a very stupid hack but there are no available pointers in
// ibv objects that I can shove my own data into.
// ================================================================
template<typename Context> struct ContextSlots {
    static inline std::array<Context*, MAX_OBJECT_OPS_SLOTS> slots{};
    static inline std::atomic<size_t>                        next_slot{0};

    // Running out of slots must DEGRADE, not abort.
    //
    // This used to std::abort(). That turns "more contexts than we sized for"
    // — a fact about the application, which a tracing tool does not get to
    // veto — into a crash of the traced program, and it would fire precisely on
    // the large real runs the tool exists to observe. Silent under-capture is
    // bad; killing the application is worse, and unlike under-capture it is not
    // recoverable by re-reading the board.
    //
    // So the caller gets kNoObjectOpsSlot and leaves that context uninstrumented
    // — the same state it was in a moment earlier — and the warning says which
    // capability was lost, once, rather than per call.
    static size_t allocate(Context* ctx) {
        auto slot = next_slot.fetch_add(1, std::memory_order_relaxed);

        if (slot >= MAX_OBJECT_OPS_SLOTS) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                std::cerr << "[comshu] object-ops slot table full (" << MAX_OBJECT_OPS_SLOTS
                          << "); further contexts will not be instrumented. Object-ops records "
                             "(post_send, poll_cq) will be missing for them.\n";
            }
            // Do not let the counter run away: it is only ever compared against
            // the ceiling, and an unbounded fetch_add would wrap eventually.
            next_slot.store(MAX_OBJECT_OPS_SLOTS, std::memory_order_relaxed);
            return kNoObjectOpsSlot;
        }

        slots[slot] = ctx;
        return slot;
    }
};

// ================================================================
// Helper to generate a table of function pointers for each slot
// This allows picking the right invoke function instantiation at runtime
// (one specialization per FuncPtr noexcept-ness, mirroring HookTrait)
// ================================================================
template<size_t MaxSlots, typename Derived, typename Tag, typename FuncPtr>
struct InvokeObjectOpsTable;

template<size_t MaxSlots, typename Derived, typename Tag, typename Ret, typename... Args>
struct InvokeObjectOpsTable<MaxSlots, Derived, Tag, Ret (*)(Args...) noexcept> {
    template<size_t... Is> static constexpr auto make_table(std::index_sequence<Is...>) noexcept {
        return std::array<Ret (*)(Args...) noexcept, sizeof...(Is)>{
            &Derived::template invoke_object_ops<Tag, Is, Args...>...};
    }
    static constexpr auto table = make_table(std::make_index_sequence<MaxSlots>{});
};

template<size_t MaxSlots, typename Derived, typename Tag, typename Ret, typename... Args>
struct InvokeObjectOpsTable<MaxSlots, Derived, Tag, Ret (*)(Args...)> {
    template<size_t... Is> static constexpr auto make_table(std::index_sequence<Is...>) noexcept {
        return std::array<Ret (*)(Args...), sizeof...(Is)>{
            &Derived::template invoke_object_ops<Tag, Is, Args...>...};
    }
    static constexpr auto table = make_table(std::make_index_sequence<MaxSlots>{});
};

// ================================================================
// Per-object context storage for object-ops hooks
// Stores original function pointers and per-object state
// ================================================================
template<typename... Tags> class ObjectContext {
public:
    // Original function pointers
    template<ValidCallbackTag Tag> auto& original() noexcept {
        constexpr size_t idx = TagIndex<Tag, Tags...>::value;
        return std::get<idx>(m_originals);
    }

    template<ValidCallbackTag Tag> HookTrait<Tag>::FuncPtr original() const noexcept {
        constexpr size_t idx = TagIndex<Tag, Tags...>::value;
        return std::get<idx>(m_originals);
    }

private:
    std::tuple<typename HookTrait<Tags>::FuncPtr...> m_originals{};
};

// ================================================================
// ObjectOpsHookManager — per-object context storage for object-ops hooks.
// CompositeHookManager::install() asks it for the object's Context (saved
// original pointers), then swaps the object's ops for slot trampolines.
// The get_op<Tag>(object) half of the contract lives on the Derived
// manager; see CompositeHookManager's usage comment.
// ================================================================
template<typename... Tags> class ObjectOpsHookManager {
public:
    using Context = ObjectContext<Tags...>;

    // ================================================================
    // Context management
    // ================================================================
    template<typename ObjectType> Context* get_context(ObjectType* obj) {
        assert(obj);

        auto        key = reinterpret_cast<uintptr_t>(obj);
        const auto& ctx_entry = m_contexts.find(key);
        if (ctx_entry != m_contexts.end()) {
            return ctx_entry->second.get();
        }

        auto ctx = std::make_unique<Context>();
        const auto& [new_ctx_entry, _] = m_contexts.emplace(key, std::move(ctx));
        return new_ctx_entry->second.get();
    }

private:
    std::unordered_map<uintptr_t, std::unique_ptr<Context>> m_contexts;
};

}  // namespace comshu::hook::detail
