#pragma once
// L1 — Flyweight interning: collapse high-cardinality values (PCs, pointers,
// strings) into dense Handles. Per-thread, per-domain — no locks on the hot
// path; tables are dumped as sidecar streams at flush and merged by pcomm-read.

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
        m_by_string.emplace(m_strings.back(), handle);
        return handle;
    }

    core::Handle intern(std::uint64_t key) {
        auto [it, inserted] = m_by_key.try_emplace(key, 0);
        if (inserted) it->second = next_handle();
        return it->second;
    }

    std::size_t size() const { return m_count; }

    // Flush support: entries in handle order. String entries own their storage.
    const std::deque<std::string>& strings() const { return m_strings; }
    const std::unordered_map<std::uint64_t, core::Handle>& keys() const { return m_by_key; }

private:
    core::Handle next_handle() { return static_cast<core::Handle>(++m_count); }

    std::size_t m_count{0};
    // std::deque (not vector): element references stay valid across growth, so
    // the string_view keys in m_by_string never dangle. (vector reallocation
    // invalidated them — passed in Release via SSO luck, aborted in Debug.)
    std::deque<std::string> m_strings;  // stable storage for string_view keys
    std::unordered_map<std::string_view, core::Handle> m_by_string;
    std::unordered_map<std::uint64_t, core::Handle> m_by_key;
};

}  // namespace pcomm::rec
