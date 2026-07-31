#pragma once
// L1 — Flyweight interning: collapse high-cardinality values (PCs, pointers,
// strings) into dense Handles, so records stay fixed-stride and the writer stays
// a memcpy.
//
// NOTE: the intended design is per-thread, per-domain with no locks on the hot
// path and a merge in the reader (UNIFICATION §3.3). That is NOT what this is.
// Today the tables are process-wide and pcomm_engine_intern_key/_str take a
// per-domain mutex per interned value — so the ibverbs firehose takes a lock per
// post_send. Unmeasured. This comment used to describe the intended design as
// though it were the current one.
//
// Tables are emitted at flush as InternKey/InternString records plus
// intern-blob.bin, and resolved by rec::InternDictionary.

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

#include "pcomm/core/ids.hpp"

namespace pcomm::rec {

enum class InternDomain : std::uint8_t {
    Pc,
    Pointer,
    String,
    KernelName,
    TypeName,
    Count_,  // number of domains, for array sizing
};

inline constexpr std::size_t kInternDomainCount =
    static_cast<std::size_t>(InternDomain::Count_);

// One table per (thread, domain). Handles are 1-based; 0 stays the null handle.
class InternTable {
public:
    core::Handle intern(std::string_view value) {
        auto it = m_by_string.find(value);
        if (it != m_by_string.end()) return it->second;
        m_strings.emplace_back(value);
        auto handle = next_handle();
        m_string_handles.push_back(handle);
        m_by_string.emplace(m_strings.back(), handle);
        return handle;
    }

    core::Handle intern(std::uint64_t key) {
        auto [it, inserted] = m_by_key.try_emplace(key, 0);
        if (inserted) it->second = next_handle();
        return it->second;
    }

    std::size_t size() const { return m_count; }

    // Flush support: entries in insertion order. String entries own their
    // storage, and carry their handle rather than letting the writer recompute
    // it. Recomputing was only correct while a domain held strings *or* keys and
    // never both — `next_handle()` is shared between them, so one key interned
    // into a string domain would have shifted every subsequent string onto the
    // wrong handle, with nothing to notice: the dictionary stays well-formed and
    // every affected record simply resolves to the wrong text.
    const std::deque<std::string>& strings() const { return m_strings; }
    const std::deque<core::Handle>& string_handles() const { return m_string_handles; }
    const std::unordered_map<std::uint64_t, core::Handle>& keys() const { return m_by_key; }

private:
    core::Handle next_handle() { return static_cast<core::Handle>(++m_count); }

    std::size_t m_count{0};
    // std::deque (not vector): element references stay valid across growth, so
    // the string_view keys in m_by_string never dangle. (vector reallocation
    // invalidated them — passed in Release via SSO luck, aborted in Debug.)
    std::deque<std::string> m_strings;  // stable storage for string_view keys
    std::deque<core::Handle> m_string_handles;  // parallel to m_strings
    std::unordered_map<std::string_view, core::Handle> m_by_string;
    std::unordered_map<std::uint64_t, core::Handle> m_by_key;
};

}  // namespace pcomm::rec
