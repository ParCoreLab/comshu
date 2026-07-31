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

#include <unistd.h>

#include "pcomm/core/records.hpp"
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
