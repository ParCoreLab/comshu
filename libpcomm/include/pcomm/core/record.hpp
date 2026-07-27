#pragma once
// L0 — RecordTrait + PCOMM_RECORD: declare a record once, and that single
// declaration drives the writer, the reader, and the Parquet schema.
// Generalizes SigmaTrace's EVENT_STRUCT / DEFINE_EVENT_TRAIT to four kinds.

#include <cstddef>
#include <map>
#include <string_view>
#include <type_traits>

#include "pcomm/core/ids.hpp"

namespace pcomm::core {

// Reads the metadata PCOMM_RECORD injects. Works for records declared in any
// namespace (providers own their record types) — no template specialization
// across namespaces required.
template <class R>
struct RecordTrait {
    static constexpr RecordKind      kind    = R::pcomm_kind;
    static constexpr RecordTypeId    type_id = R::pcomm_type_id;
    static constexpr std::string_view name   = R::pcomm_name;
    static constexpr std::size_t size() { return sizeof(R); }
};

template <class R>
concept Record = std::is_trivially_copyable_v<R> && requires {
    { RecordTrait<R>::kind } -> std::convertible_to<RecordKind>;
    { RecordTrait<R>::type_id } -> std::convertible_to<RecordTypeId>;
    { RecordTrait<R>::name } -> std::convertible_to<std::string_view>;
};

// Empty base carrying the trait metadata; records stay aggregates (C++17
// aggregates may have public bases) and trivially copyable.
template <RecordKind K, RecordTypeId Id>
struct RecordMeta {
    static constexpr RecordKind   pcomm_kind    = K;
    static constexpr RecordTypeId pcomm_type_id = Id;
};

// Runtime duplicate-id detection (successor of SigmaTrace's id tracker).
// Registration happens lazily, the first time a record type reaches a Recorder.
class RecordIdTracker {
public:
    static RecordIdTracker& instance();  // src/engine.cpp — one per process

    // Returns false if the id is already claimed by a *different* name.
    bool claim(RecordTypeId id, std::string_view name) {
        auto [it, inserted] = m_names.emplace(id, name);
        return inserted || it->second == name;
    }

private:
    std::map<RecordTypeId, std::string_view> m_names;
};

}  // namespace pcomm::core

// PCOMM_RECORD(Struct, Kind, TypeId, "name") { ...fields... };
//
// Records must stay trivially-copyable aggregates: the writer is a memcpy and
// the reader maps streams as arrays. High-cardinality fields (pc, pointers,
// strings, names) must be core::Handle (interned), never raw values.
#define PCOMM_RECORD(Struct, Kind, TypeId, NameStr)                           \
    struct Struct;                                                            \
    struct Struct##Name_ {                                                    \
        static constexpr std::string_view pcomm_name = (NameStr);             \
    };                                                                        \
    struct Struct : ::pcomm::core::RecordMeta<(Kind), (TypeId)>, Struct##Name_
