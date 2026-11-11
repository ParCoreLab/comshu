// InternTable, path by path.
//
// This is the flyweight every Handle in every record depends on, and its
// failures are quiet by construction: a wrong handle is still a valid handle, so
// a record resolves to the wrong text rather than to an error. The engine-level
// round trip is tests/intern_format.cpp; this covers the table itself, where the
// interesting cases live and where no NIC or engine is needed to reach them.
//
// Two of these guard bugs this code has actually had:
//   - storage stability, which was a vector and dangled its own lookup keys
//   - the handle counter shared between strings and keys

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "comshu/rec/intern_table.hpp"

using comshu::core::Handle;
using comshu::core::kNullHandle;
using comshu::rec::InternTable;

namespace {

int g_fail = 0;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok " : "XX ", what);
    if (!ok)
        g_fail = 1;
}

}  // namespace

int main() {
    std::printf("intern table: the dictionary behind every Handle\n");

    // --- identity ------------------------------------------------------------
    {
        InternTable t;
        const auto  a = t.intern("foo.cu");
        const auto  b = t.intern("bar.cu");
        const auto  c = t.intern("foo.cu");
        expect(a == c, "the same string interns to the same handle");
        expect(a != b, "different strings get different handles");
        expect(t.size() == 2, "and re-interning adds no entry");
    }
    {
        InternTable t;
        const auto  a = t.intern(std::uint64_t{0x1f80});
        const auto  b = t.intern(std::uint64_t{0x1f81});
        expect(a == t.intern(std::uint64_t{0x1f80}), "same for numeric keys");
        expect(a != b && t.size() == 2, "distinct keys, two entries");
    }

    // --- 0 is the null handle and must never be issued -----------------------
    {
        InternTable t;
        expect(t.intern("") != kNullHandle,
               "the empty string is a value, not an absence — it gets a handle");
        expect(t.intern(std::uint64_t{0}) != kNullHandle,
               "and so does the key 0, which is a legitimate pc/pointer value");
        expect(t.intern("") == 1, "handles are 1-based, so 0 stays reserved for null");
    }

    // --- the counter is shared between strings and keys ----------------------
    // The writer used to recompute string handles by counting strings, which is
    // only correct while a domain holds one kind or the other. A key interned
    // between two strings shifts every later string onto the wrong handle, and
    // the resulting dictionary is perfectly well-formed — every affected record
    // just resolves to the wrong text.
    {
        InternTable t;
        const auto  s1 = t.intern("first");
        const auto  k1 = t.intern(std::uint64_t{111});
        const auto  s2 = t.intern("second");
        expect(s1 == 1 && k1 == 2 && s2 == 3,
               "strings and keys draw from one counter, in call order");
        expect(t.strings().size() == 2 && t.string_handles().size() == 2,
               "the string side keeps two entries");
        expect(t.string_handles()[0] == s1 && t.string_handles()[1] == s2,
               "each carrying its real handle, not its position — position would "
               "have said 1 and 2, and 2 belongs to the key");
        expect(t.keys().at(111) == k1, "and the key keeps its own");
        expect(t.size() == 3, "size counts both kinds");
    }

    // --- storage stability, the bug this container choice exists for ---------
    // m_by_string is keyed by string_view into m_strings. With a vector, growth
    // reallocates and every one of those views dangles; it "passed in Release via
    // SSO luck" and aborted in Debug. A deque never moves its elements.
    //
    // The decisive probe is re-interning the *earliest* strings after many
    // reallocations would have happened: with dangling keys the lookup compares
    // against freed memory, misses, and issues a second handle for a string that
    // is already there — so the count grows and the handle changes.
    {
        InternTable         t;
        constexpr int       kN = 2000;
        std::vector<Handle> handles;
        handles.reserve(kN);
        for (int i = 0; i < kN; ++i) {
            // Long enough that no small-string optimisation keeps the bytes
            // inside the std::string object, so the view really points at heap
            // storage that a vector would have moved.
            handles.push_back(
                t.intern("a-deliberately-long-interned-name-well-past-SSO-" + std::to_string(i)));
        }
        expect(t.size() == kN, "every distinct string is stored once");

        bool stable = true;
        for (int i = 0; i < kN; ++i)
            stable &= t.intern("a-deliberately-long-interned-name-well-past-SSO-" +
                               std::to_string(i)) == handles[i];
        expect(stable, "and re-interning any of them after growth returns the "
                       "original handle — the lookup keys still point at live bytes");
        expect(t.size() == kN, "with no duplicates created by a dangling key");

        std::set<Handle> distinct(handles.begin(), handles.end());
        expect(distinct.size() == static_cast<std::size_t>(kN),
               "and no two distinct strings ever shared a handle");
        expect(*distinct.begin() == 1 && *distinct.rbegin() == kN,
               "handles are dense: 1..N with no gaps");
    }

    // --- prefixes, and the NUL trap -----------------------------------------
    {
        InternTable t;
        const auto  a = t.intern("kernel");
        const auto  b = t.intern("kernel_name");
        expect(a != b && t.size() == 2, "a prefix is a different string");

        // A char* literal reaches string_view through the strlen constructor, so
        // everything past an embedded NUL is silently dropped and this is just
        // "kernel" again. Worth pinning: a caller interning a symbol name out of
        // a buffer gets truncation with no error, and the handle it gets back is
        // some *other* string's.
        expect(t.intern("kernel\0hidden") == a,
               "a literal with an embedded NUL truncates at the NUL — same handle "
               "as the prefix, not a new entry");
        expect(t.size() == 2, "and nothing new is stored");

        // With an explicit length the bytes are kept and it is its own string.
        const auto c = t.intern(std::string_view("kernel\0hidden", 13));
        expect(c != a && t.size() == 3,
               "passed with a length, the embedded NUL is data and interns "
               "separately");
        expect(t.intern(std::string_view("kernel\0hidden", 13)) == c,
               "and matches by length, not by terminator");
    }

    // --- insertion order is what the writer serialises ----------------------
    {
        InternTable t;
        t.intern("one");
        t.intern("two");
        t.intern("three");
        const auto& s = t.strings();
        expect(s.size() == 3 && s[0] == "one" && s[1] == "two" && s[2] == "three",
               "strings() yields insertion order, which is the order the blob is "
               "written in");
    }

    // --- a fresh table is empty ---------------------------------------------
    {
        InternTable t;
        expect(t.size() == 0 && t.strings().empty() && t.keys().empty(),
               "a new table has nothing in it, and no handle 0 to hand out");
    }

    std::printf("%s\n", g_fail ? "FAIL" : "PASS — intern table paths hold");
    return g_fail;
}
