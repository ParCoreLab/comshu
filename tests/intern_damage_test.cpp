// CR-39: a damaged intern index must be refused entry by entry, never trusted
// into substr(). An offset near 2^64 made `off + len` wrap below the blob size,
// pass the bounds check, and throw std::out_of_range out of the reader — so one
// corrupt record cost the whole capture's names.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "comshu/core/records.hpp"
#include "comshu/rec/intern_reader.hpp"
#include "comshu/rec/stream_header.hpp"
#include "comshu/tracer.hpp"

namespace fs = std::filesystem;
using namespace comshu;

static std::size_t g_diags = 0;  // StreamDiagnostic is a plain function pointer
static void count_diag(const fs::path&, const std::string&) { ++g_diags; }

int main() {
    const fs::path out = fs::temp_directory_path() / ("comshu-interndmg-" + std::to_string(::getpid()));
    fs::remove_all(out);
    Config cfg;
    cfg.output_dir = out;
    cfg.rank = 0;
    Tracer::instance().start(cfg);
    auto&              r = Tracer::instance().recorder();
    const core::Handle a = r.intern(rec::InternDomain::String, "alpha");
    const core::Handle b = r.intern(rec::InternDomain::String, "beta");
    Tracer::instance().stop();
    const fs::path dir = out / "rank-0";

    // Damage the FIRST index entry: an offset that wraps when the length is added.
    const auto path = rec::stream_path<core::records::InternString>(dir);
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    rec::Stamped<core::records::InternString> s{};
    f.seekg(sizeof(rec::StreamHeader));
    if (!f.read(reinterpret_cast<char*>(&s), sizeof s)) {
        std::puts("intern_damage: FAIL — no intern_string records written");
        return 1;
    }
    const core::Handle damaged = s.payload.handle;
    s.payload.offset = ~std::uint64_t{0} - 3;  // + length wraps
    f.seekp(sizeof(rec::StreamHeader));
    f.write(reinterpret_cast<const char*>(&s), sizeof s);
    f.close();

    try {
        const auto  dict = rec::InternDictionary::load(dir, &count_diag);
        const auto  diags = g_diags;
        const core::Handle intact = damaged == a ? b : a;
        const char*        want = intact == a ? "alpha" : "beta";
        const bool ok = std::string(dict.str(rec::InternDomain::String, intact)) == want &&
                        std::string(dict.str(rec::InternDomain::String, damaged)).empty() &&
                        diags > 0;
        fs::remove_all(out);
        std::printf("intern_damage: %s (intact name resolved, damaged one refused, %zu diag)\n",
                    ok ? "ok" : "FAIL", diags);
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        fs::remove_all(out);
        std::printf("intern_damage: FAIL — the reader threw: %s\n", e.what());
        return 1;
    }
}
