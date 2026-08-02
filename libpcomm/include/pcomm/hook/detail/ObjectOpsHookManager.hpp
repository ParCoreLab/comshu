#pragma once

#include <cassert>

#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <unordered_map>

#include "pcomm/hook/HookTrait.hpp"

#include "pcomm/hook/detail/TagIndex.hpp"

namespace pcomm::hook::detail {
    // ================================================================
    // Maximum number of object-ops contexts we can track per Tag
    static constexpr size_t MAX_OBJECT_OPS_SLOTS = 256;

    // ================================================================
    // Slot-based storage for context pointers to avoid find() for each op
    // Each Tag gets its own set of slots to store Context pointers
    // This is a very stupid hack but there are no available pointers in
    // ibv objects that I can shove my own data into.
    // ================================================================
    template<typename Context>
    struct ContextSlots {
        static inline std::array<Context*, MAX_OBJECT_OPS_SLOTS> slots{};
        static inline std::atomic<size_t> next_slot{0};

        static size_t allocate(Context* ctx) {
            auto slot = next_slot.fetch_add(1, std::memory_order_relaxed);

            // Hopefully no one has 257 NICs in one node...
            if (slot >= MAX_OBJECT_OPS_SLOTS) {
                std::cerr << "Too many object-ops contexts. This shouldn't happen!" << std::endl;
                std::abort();
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
        template<size_t... Is>
        static constexpr auto make_table(std::index_sequence<Is...>) noexcept {
            return std::array<Ret (*)(Args...) noexcept, sizeof...(Is)>{
                    &Derived::template invoke_object_ops<Tag, Is, Args...>...};
        }
        static constexpr auto table = make_table(std::make_index_sequence<MaxSlots>{});
    };

    template<size_t MaxSlots, typename Derived, typename Tag, typename Ret, typename... Args>
    struct InvokeObjectOpsTable<MaxSlots, Derived, Tag, Ret (*)(Args...)> {
        template<size_t... Is>
        static constexpr auto make_table(std::index_sequence<Is...>) noexcept {
            return std::array<Ret (*)(Args...), sizeof...(Is)>{
                    &Derived::template invoke_object_ops<Tag, Is, Args...>...};
        }
        static constexpr auto table = make_table(std::make_index_sequence<MaxSlots>{});
    };

    // ================================================================
    // Per-object context storage for object-ops hooks
    // Stores original function pointers and per-object state
    // ================================================================
    template<typename... Tags>
    class ObjectContext {
    public:
        // Original function pointers
        template<ValidCallbackTag Tag>
        auto& original() noexcept {
            constexpr size_t idx = TagIndex<Tag, Tags...>::value;
            return std::get<idx>(m_originals);
        }

        template<ValidCallbackTag Tag>
        HookTrait<Tag>::FuncPtr original() const noexcept {
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
    template<typename... Tags>
    class ObjectOpsHookManager {
    public:
        using Context = ObjectContext<Tags...>;

        // ================================================================
        // Context management
        // ================================================================
        template<typename ObjectType>
        Context* get_context(ObjectType* obj) {
            assert(obj);

            auto key = reinterpret_cast<uintptr_t>(obj);
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

} // namespace pcomm::hook::detail
