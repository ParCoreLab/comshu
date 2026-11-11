#pragma once
// L0 — RecordTrait + COMSHU_RECORD: declare a record once, and that single
// declaration drives the writer, the reader, and the Parquet schema.
// Generalizes SigmaTrace's EVENT_STRUCT / DEFINE_EVENT_TRAIT to four kinds.

#include <cstddef>
#include <map>
#include <string_view>
#include <type_traits>

#include "comshu/core/ids.hpp"

namespace comshu::core {

// Reads the metadata COMSHU_RECORD injects. Works for records declared in any
// namespace (providers own their record types) — no template specialization
// across namespaces required.
template<class R> struct RecordTrait {
    static constexpr RecordKind       kind = R::comshu_kind;
    static constexpr RecordTypeId     type_id = R::comshu_type_id;
    static constexpr std::string_view name = R::comshu_name;
    // Records with the same `name` are one *family*; `version` orders them.
    // See COMSHU_RECORD_V below for what a version actually means.
    static constexpr std::uint16_t version = R::comshu_version;
    static constexpr std::size_t   size() { return sizeof(R); }
};

template<class R>
concept Record = std::is_trivially_copyable_v<R> && requires {
    { RecordTrait<R>::kind } -> std::convertible_to<RecordKind>;
    { RecordTrait<R>::type_id } -> std::convertible_to<RecordTypeId>;
    { RecordTrait<R>::name } -> std::convertible_to<std::string_view>;
};

// Empty base carrying the trait metadata; records stay aggregates (C++17
// aggregates may have public bases) and trivially copyable.
template<RecordKind K, RecordTypeId Id, std::uint16_t Ver = 1> struct RecordMeta {
    static constexpr RecordKind    comshu_kind = K;
    static constexpr RecordTypeId  comshu_type_id = Id;
    static constexpr std::uint16_t comshu_version = Ver;
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

}  // namespace comshu::core

// COMSHU_RECORD(Struct, Kind, TypeId, "name") { ...fields... };
//
// Records must stay trivially-copyable aggregates: the writer is a memcpy and
// the reader maps streams as arrays. High-cardinality fields (pc, pointers,
// strings, names) must be core::Handle (interned), never raw values.
#define COMSHU_RECORD(Struct, Kind, TypeId, NameStr) COMSHU_RECORD_V(Struct, Kind, TypeId, NameStr, 1)

// COMSHU_RECORD_V(Struct, Kind, TypeId, "name", Version) — the same, with an
// explicit schema version. This is how a record evolves.
//
// THE CONVENTION, because there was not one:
//
// A record's layout is immutable the moment a capture containing it exists.
// FlatBuffers structs cannot gain fields, and even if they could, the files
// already written would not gain them — so "add a field to IbPostSend" is never
// an edit. It is a *new record type*: same `name`, next `Version`, and a fresh
// `TypeId`.
//
//   COMSHU_RECORD  (IbPostSend,   Event, 110, "ib_post_send")        // v1
//   COMSHU_RECORD_V(IbPostSendV2, Event, 114, "ib_post_send", 2)     // v2
//
// Records sharing a `name` are one **family**; `Version` orders them. The name
// is the stable identity — it is what the CSV table, the goldens and every
// query are written against — while the type_id is the on-disk discriminator
// and never means two things.
//
// The type_id is simply the next free id in the provider's range. There is
// deliberately no arithmetic rule tying v2's id to v1's (no "+1", no reserved
// low digit): the ids already allocated are irregularly spaced, so any such
// rule would be a retrofit that is false for the existing records and would run
// out of room the first time it mattered. The link between versions lives in
// the declaration, where it can be checked, not in the arithmetic of the
// number, where it can only be assumed.
//
// Rules, all enforced by tools/schema_check.py:
//   - versions within a family start at 1 and are contiguous
//   - (name, version) is unique, and so is type_id
//   - every version of a family has the same RecordKind
//   - an older version is never edited or deleted; captures still hold it
//
// The reader's side is comshu/rec/version_map.hpp: comshu-read loads every
// version of a family and upconverts the old ones, so a capture written before
// the bump and one written after produce the same table.
#define COMSHU_RECORD_V(Struct, Kind, TypeId, NameStr, Version)                                     \
    struct Struct;                                                                                 \
    struct Struct##Name_ {                                                                         \
        static constexpr std::string_view comshu_name = (NameStr);                                  \
    };                                                                                             \
    struct Struct : ::comshu::core::RecordMeta<(Kind), (TypeId), (Version)>, Struct##Name_
