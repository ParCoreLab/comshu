// The intern dictionary, written by the real capture path and read back by the
// real reader.
//
// A Handle is meaningless without this dictionary, so its failure modes matter
// more than most: every one of them voids the entire capture rather than one
// record. The old sidecars were bare TSV with no magic, no version and no
// schema, and the lookup answered "" for anything missing — so a truncated,
// absent or foreign dictionary produced empty cells and no error, which is
// indistinguishable from a capture that simply had no strings.
//
// This writes through Tracer (the path a real run takes, engine and all) and
// reads through rec::InternDictionary (the path pcomm-read takes), then breaks
// the capture four ways and checks each is reported.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <atomic>
#include <thread>
#include <unordered_map>
#include <unistd.h>

#include "pcomm/core/records.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/rec/intern_reader.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/tracer.hpp"

namespace fs = std::filesystem;
using namespace pcomm;
using rec::InternDomain;

namespace {

int g_fail = 0;
std::vector<std::string> g_diags;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok " : "XX ", what);
    if (!ok) g_fail = 1;
}

void collect(const fs::path& p, const std::string& msg) {
    g_diags.push_back(p.filename().string() + ": " + msg);
}

bool diag_mentions(const char* needle) {
    for (const auto& d : g_diags)
        if (d.find(needle) != std::string::npos) return true;
    return false;
}

void dump_diags() {
    for (const auto& d : g_diags) std::printf("         -> %s\n", d.c_str());
}

}  // namespace

int main() {
    std::printf("intern dictionary: real capture path, real reader\n");

    const auto root = fs::temp_directory_path() /
                      ("pcomm-intern-" + std::to_string(::getpid()));
    fs::remove_all(root);

    // --- interning before there is anywhere to put it ------------------------
    // Providers are loaded by LD_PRELOAD and can be called before (or after) the
    // Tracer holds a recorder — a hooked function on a library's own init path
    // is enough. The engine answers kNullHandle rather than touching a null
    // recorder, and 0 is exactly the value records already use for "no value",
    // so such a record degrades to unattributed instead of crashing the target.
    // This must run first: once the Tracer starts there is no way back.
    {
        const auto h_str = pcomm_engine_intern_str(
            static_cast<std::uint8_t>(InternDomain::String), "before-start", 12);
        const auto h_key = pcomm_engine_intern_key(
            static_cast<std::uint8_t>(InternDomain::Pc), 0xDEAD);
        expect(h_str == 0 && h_key == 0,
               "interning before the tracer has a recorder yields the null handle, "
               "not a crash and not a plausible handle");
    }

    // --- write a capture the way a run does ----------------------------------
    Config cfg;
    cfg.output_dir = root;
    cfg.rank = 0;
    Tracer::instance().start(cfg);
    auto& rec = Tracer::instance().recorder();

    const auto file_h   = rec.intern(InternDomain::String, "stencil.cu");
    const auto file2_h  = rec.intern(InternDomain::String, "reduce.cu");
    const auto same_h   = rec.intern(InternDomain::String, "stencil.cu");
    const auto kernel_h = rec.intern(InternDomain::KernelName,
                                     "_Z6kernelPfS_i_with_a_deliberately_long_mangled_name");
    // Deliberately mixed into one domain: InternTable shares its handle counter
    // between strings and keys, so interning a key here shifts the handle of
    // every string interned after it. A writer that recomputed handles by
    // counting strings would produce a perfectly well-formed dictionary in
    // which those strings resolve to the wrong text.
    const auto mixed_key_h = rec.intern(InternDomain::String, std::uint64_t{0xABCDEF});
    const auto after_mix_h = rec.intern(InternDomain::String, "after_the_key.cu");

    const auto pc_h     = rec.intern(InternDomain::Pc, std::uint64_t{0x7fff'1234'5678});
    const auto ptr_h    = rec.intern(InternDomain::Pointer, std::uint64_t{0x55db'6e39'7220});

    expect(file_h == same_h, "interning the same string twice yields one handle");

    // --- many threads, one dictionary ---------------------------------------
    // The tables are process-wide behind a per-domain mutex (see
    // intern_table.hpp — the lock-free per-thread design is still only intended).
    // What must hold is that concurrent interning is *consistent*: one handle per
    // distinct value, and no handle serving two values. A race here does not
    // crash, it quietly gives two threads different handles for the same string,
    // and every record from one of them then resolves to the wrong text.
    //
    // Uses TypeName, which nothing else in this test touches, so the assertions
    // above and below are unaffected by what the threads add.
    {
        constexpr int kThreads = 8, kPerThread = 200;
        std::vector<std::vector<core::Handle>> got(kThreads);
        std::vector<std::thread> workers;
        // Released only once every thread is up, so they arrive at the table
        // together rather than staggered. Measured by deleting the engine's
        // lock_guard and rerunning: without this barrier the check caught the
        // unlocked build in 2 runs out of 3; with it, 6 out of 6 (three of those
        // as outright segfaults from concurrent unordered_map insertion), while
        // the locked build passed 6 out of 6.
        //
        // Still a probabilistic check — a race that does not happen cannot be
        // observed — so a pass means "no inconsistency seen", not "locking is
        // proven correct".
        std::atomic<bool> go{false};
        for (int t = 0; t < kThreads; ++t)
            workers.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) { /* spin */ }
                for (int i = 0; i < kPerThread; ++i)
                    // Overlapping ranges on purpose: every value is interned by
                    // several threads, which is the contended case.
                    got[t].push_back(rec.intern(InternDomain::TypeName,
                                                "shared_type_" + std::to_string(i % 50)));
            });
        go.store(true, std::memory_order_release);
        for (auto& w : workers) w.join();

        std::unordered_map<int, core::Handle> by_value;
        std::unordered_map<core::Handle, int> by_handle;
        bool consistent = true;
        for (int t = 0; t < kThreads; ++t)
            for (int i = 0; i < kPerThread; ++i) {
                const int value = i % 50;
                const auto h = got[t][i];
                consistent &= h != 0;
                auto [vit, v_new] = by_value.emplace(value, h);
                consistent &= vit->second == h;          // one handle per value
                auto [hit, h_new] = by_handle.emplace(h, value);
                consistent &= hit->second == value;      // one value per handle
            }
        expect(consistent, "8 threads interning overlapping values agree: one "
                           "handle per value, one value per handle");
        expect(by_value.size() == 50,
               "and the 50 distinct values produced exactly 50 handles");
    }

    rec.record(core::Temporal::none(),
               core::records::SourceLine{{}, {}, pc_h, file_h, 42});
    Tracer::instance().finalize();

    const auto rank_dir = root / "rank-0";
    expect(fs::exists(rank_dir / "512.pcs") && fs::exists(rank_dir / "intern-blob.bin"),
           "the dictionary is a record stream plus a framed blob, not a bare TSV");
    expect(!fs::exists(rank_dir / "intern-string.tsv"),
           "and the unversioned TSV sidecar is gone, not written alongside");

    // --- read it back --------------------------------------------------------
    {
        g_diags.clear();
        auto dict = rec::InternDictionary::load(rank_dir, collect);
        if (!g_diags.empty()) dump_diags();
        expect(g_diags.empty(), "a well-formed dictionary loads without complaint");
        expect(std::string(dict.str(InternDomain::String, file_h)) == "stencil.cu" &&
                   std::string(dict.str(InternDomain::String, file2_h)) == "reduce.cu",
               "strings resolve to what was interned");
        expect(std::string(dict.str(InternDomain::KernelName, kernel_h)) ==
                   "_Z6kernelPfS_i_with_a_deliberately_long_mangled_name",
               "including a long one, so the blob is not fixed-width in disguise");

        // The bug the (domain, handle) key exists to prevent: handles restart at
        // 1 in every domain, so a single-keyed map would have these collide.
        expect(kernel_h == file_h,
               "handles do collide across domains — both domains start at 1");
        expect(std::string(dict.str(InternDomain::String, file_h)) !=
                   std::string(dict.str(InternDomain::KernelName, kernel_h)),
               "but they resolve to different strings, because the key is "
               "(domain, handle)");

        expect(std::string(dict.str(InternDomain::String, after_mix_h)) ==
                   "after_the_key.cu",
               "a string interned after a key in the same domain still resolves "
               "— handles come from the table, not from counting");

        std::uint64_t v = 0;
        expect(dict.value(InternDomain::String, mixed_key_h, v) && v == 0xABCDEF,
               "and the key sharing that domain resolves too");
        expect(dict.value(InternDomain::Pc, pc_h, v) && v == 0x7fff'1234'5678,
               "numeric handles resolve to their raw value");
        expect(dict.value(InternDomain::Pointer, ptr_h, v) && v == 0x55db'6e39'7220,
               "pointers too, from the same stream");
        expect(!dict.value(InternDomain::Pc, 9999, v),
               "an unknown handle says so rather than returning a plausible 0");
    }

    // --- now break it, four ways ---------------------------------------------
    auto corrupt = [&](const char* label, auto&& mutate, const char* needle,
                       const char* what) {
        const auto broken = root / label;
        fs::remove_all(broken);
        fs::copy(rank_dir, broken, fs::copy_options::recursive);
        mutate(broken);
        g_diags.clear();
        auto dict = rec::InternDictionary::load(broken, collect);
        const bool reported = diag_mentions(needle);
        expect(reported, what);
        if (!reported) dump_diags();
        return dict;
    };

    corrupt("missing-blob",
            [](const fs::path& d) { fs::remove(d / "intern-blob.bin"); },
            "missing",
            "a dictionary whose blob is gone is reported, not silently empty");

    corrupt("truncated-blob",
            [](const fs::path& d) {
                fs::resize_file(d / "intern-blob.bin",
                                fs::file_size(d / "intern-blob.bin") - 8);
            },
            "truncated",
            "a truncated blob is caught by byte_count, not by luck");

    corrupt("foreign-blob",
            [](const fs::path& d) {
                std::FILE* f = std::fopen((d / "intern-blob.bin").c_str(), "r+b");
                rec::BlobHeader h{};
                std::fread(&h, sizeof h, 1, f);
                h.magic = 0;                       // some other file entirely
                std::fseek(f, 0, SEEK_SET);
                std::fwrite(&h, sizeof h, 1, f);
                std::fclose(f);
            },
            "magic",
            "a blob that is not a pcomm blob is refused by magic");

    {   // An index from a different (longer) run against this blob: offsets are
        // plausible integers, and nothing but a bounds check notices.
        auto dict = corrupt("stale-index",
            [](const fs::path& d) {
                std::FILE* f = std::fopen((d / "intern-blob.bin").c_str(), "r+b");
                rec::BlobHeader h{};
                std::fread(&h, sizeof h, 1, f);
                h.byte_count = 4;                  // blob shrank; index did not
                std::fseek(f, 0, SEEK_SET);
                std::fwrite(&h, sizeof h, 1, f);
                std::fclose(f);
                fs::resize_file(d / "intern-blob.bin", sizeof h + 4);
            },
            "past the end",
            "an index pointing past a self-consistent but smaller blob is "
            "reported as an index/blob mismatch");
        expect(dict.string_count() == 0,
               "and yields nothing rather than substrings of the wrong text");
    }

    fs::remove_all(root);
    std::printf("%s\n", g_fail
        ? "FAIL"
        : "PASS — the dictionary round-trips, and every way of breaking it is reported");
    return g_fail;
}
