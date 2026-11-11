// Proves the record-versioning convention: a capture written before a schema
// bump and one written after both read, through the same code comshu-read uses.
//
// The convention (COMSHU_RECORD_V in core/record.hpp) says a record's layout is
// frozen once a capture holds it, so evolving one means a new type_id at the
// next version in the same family. On its own that is a statement about
// writing. What makes it a *mechanism* is that a reader handed both versions
// produces one table — which is what this checks.
//
// The demo family lives here rather than in the schema on purpose. Inventing a
// field that no provider wants, just to have something to bump, would put a
// permanent fiction in the on-disk format; ids 900+ are outside every range
// allocated in core/ids.hpp, and tools/schema_check.py exempts test directories
// precisely so a fixture cannot masquerade as a real record type.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include "comshu/core/record.hpp"
#include "comshu/core/records.hpp"
#include "comshu/rec/sink.hpp"
#include "comshu/rec/stream_reader.hpp"
#include "comshu/rec/thread_local_buffer.hpp"
#include "comshu/rec/version_map.hpp"

namespace fs = std::filesystem;
using namespace comshu;
using rec::Stamped;

namespace demo {
using core::DeviceId;
using core::Handle;
using core::RecordKind;

// v1 — what the older build wrote, and what its captures still hold.
COMSHU_RECORD(ThingV1, RecordKind::Event, 900, "demo_thing") {
    Handle        id;
    std::uint32_t value;
};

// v2 — the same family, one version on, with a field v1 never recorded.
COMSHU_RECORD_V(ThingV2, RecordKind::Event, 901, "demo_thing", 2) {
    Handle        id;
    std::uint32_t value;
    DeviceId      device;
};

// The device a v1 record was written on is *not zero* — zero is a real device.
// It is unknown, and the upgrade has to be able to say so, or every query over
// a mixed capture silently attributes old records to device 0.
inline constexpr core::DeviceId kDeviceUnknown = 0xFFFF;

ThingV2 upgrade_v1(const ThingV1& old) {
    return ThingV2{{}, {}, old.id, old.value, kDeviceUnknown};
}
}  // namespace demo

namespace {

int g_fail = 0;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok " : "XX ", what);
    if (!ok)
        g_fail = 1;
}

// Write `n` records of one type into a capture directory, through the real sink.
template<core::Record R, class Fill>
void write_stream(const fs::path& dir, std::size_t n, Fill fill) {
    std::vector<Stamped<R>> recs(n);
    for (std::size_t i = 0; i < n; ++i) {
        recs[i].temporal = core::Temporal::instant({1, 1000 + i});
        fill(recs[i].payload, i);
    }
    rec::FileSink sink(dir);
    sink.submit({core::RecordTrait<R>::type_id, core::RecordTrait<R>::kind,
                 reinterpret_cast<const std::byte*>(recs.data()), recs.size() * sizeof(Stamped<R>),
                 recs.size()});
}

// Every version of the demo family in one directory, as v2. This is the shape
// a real reader takes: one call site per family, one upgrade per step.
std::vector<Stamped<demo::ThingV2>> load_things(const fs::path& dir, rec::StreamDiagnostic diag) {
    auto load = [&]<class R>() { return rec::load_stream<R>(rec::stream_path<R>(dir), diag); };
    std::vector<Stamped<demo::ThingV2>> out;
    rec::load_version<demo::ThingV2, demo::ThingV1>(load, demo::upgrade_v1, out);
    rec::load_current<demo::ThingV2>(load, out);
    return out;
}

std::vector<std::string> g_diags;
void                     collect(const fs::path& p, const std::string& msg) {
    g_diags.push_back(p.filename().string() + ": " + msg);
}

}  // namespace

int main() {
    std::printf("record versioning: one family, two schema versions\n");

    const auto root =
        fs::temp_directory_path() / ("comshu-versioning-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const auto old_capture = root / "written-by-v1";
    const auto new_capture = root / "written-by-v2";

    // Two captures from two builds of the tool, which is the situation the
    // convention exists for.
    write_stream<demo::ThingV1>(old_capture, 5, [](demo::ThingV1& r, std::size_t i) {
        r.id = static_cast<core::Handle>(i + 1);
        r.value = static_cast<std::uint32_t>(100 + i);
    });
    write_stream<demo::ThingV2>(new_capture, 3, [](demo::ThingV2& r, std::size_t i) {
        r.id = static_cast<core::Handle>(i + 1);
        r.value = static_cast<std::uint32_t>(200 + i);
        r.device = 7;
    });

    expect(fs::exists(old_capture / "900.pcs") && fs::exists(new_capture / "901.pcs"),
           "each version writes its own type_id, so neither file can shadow the other");

    // --- the old capture still reads, as the current type ---------------------
    {
        g_diags.clear();
        auto things = load_things(old_capture, collect);
        expect(things.size() == 5, "all 5 v1 records read from the pre-bump capture");
        expect(g_diags.empty(), "and no diagnostics: an old capture is not an error");

        bool values_ok = true, temporal_ok = true, device_ok = true;
        for (std::size_t i = 0; i < things.size(); ++i) {
            values_ok &= things[i].payload.id == i + 1 && things[i].payload.value == 100 + i;
            temporal_ok &= things[i].temporal.a.ticks == 1000 + i &&
                           things[i].temporal.kind == core::TimeKind::Instant;
            device_ok &= things[i].payload.device == demo::kDeviceUnknown;
        }
        expect(values_ok, "fields v1 did carry survive the upgrade unchanged");
        expect(temporal_ok, "the temporal stamp is carried, not recomputed");
        expect(device_ok, "the field v1 never recorded reads as unknown, not as 0 "
                          "— device 0 is a real device");
    }

    // --- the new capture reads as itself --------------------------------------
    {
        g_diags.clear();
        auto things = load_things(new_capture, collect);
        expect(things.size() == 3, "all 3 v2 records read from the post-bump capture");
        bool ok = true;
        for (std::size_t i = 0; i < things.size(); ++i)
            ok &= things[i].payload.device == 7 && things[i].payload.value == 200 + i;
        expect(ok, "v2 records keep the field they actually recorded");
        expect(g_diags.empty(), "no diagnostics for a current capture");
    }

    // --- a directory holding both, which is what a merged run looks like ------
    {
        const auto mixed = root / "mixed";
        write_stream<demo::ThingV1>(mixed, 2, [](demo::ThingV1& r, std::size_t i) {
            r.id = static_cast<core::Handle>(i + 1);
            r.value = 10;
        });
        write_stream<demo::ThingV2>(mixed, 4, [](demo::ThingV2& r, std::size_t i) {
            r.id = static_cast<core::Handle>(i + 1);
            r.value = 20;
            r.device = 3;
        });
        g_diags.clear();
        auto things = load_things(mixed, collect);
        expect(things.size() == 6, "both versions in one directory read into one table");
        std::size_t unknown = 0, known = 0;
        for (const auto& t : things)
            (t.payload.device == demo::kDeviceUnknown ? unknown : known)++;
        expect(unknown == 2 && known == 4, "and each record keeps what its own version knew");
    }

    // --- a stream from a build newer than this one ----------------------------
    // The failure this prevents: a v3 arrives, no code names type 902, the file
    // is skipped, and the export is quietly short. Nothing errors — which is
    // exactly why it has to be reported.
    {
        const auto future = root / "written-by-v3";
        write_stream<demo::ThingV2>(future, 1, [](demo::ThingV2& r, std::size_t) {
            r.id = 1;
            r.value = 1;
            r.device = 0;
        });
        // Same bytes, a type_id this build has never heard of.
        fs::rename(future / "901.pcs", future / "902.pcs");
        {  // rewrite the header's type_id so the file is self-consistently v3
            rec::StreamHeader h{};
            std::FILE*        f = std::fopen((future / "902.pcs").c_str(), "r+b");
            std::fread(&h, sizeof h, 1, f);
            h.type_id = 902;
            std::fseek(f, 0, SEEK_SET);
            std::fwrite(&h, sizeof h, 1, f);
            std::fclose(f);
        }

        g_diags.clear();
        auto things = load_things(future, collect);
        expect(things.empty(), "a version this build cannot name yields no rows");

        auto unhandled =
            rec::unhandled_streams(future, {core::RecordTrait<demo::ThingV1>::type_id,
                                            core::RecordTrait<demo::ThingV2>::type_id});
        expect(unhandled.size() == 1 && unhandled[0].type_id == 902,
               "but the reader reports it instead of exporting a short table");
        if (unhandled.size() == 1)
            std::printf("         -> %s\n", unhandled[0].message.c_str());
    }

    // --- a known stream is not reported as unhandled --------------------------
    {
        auto unhandled =
            rec::unhandled_streams(new_capture, {core::RecordTrait<demo::ThingV1>::type_id,
                                                 core::RecordTrait<demo::ThingV2>::type_id});
        expect(unhandled.empty(), "a stream this build does handle is not flagged");
    }

    fs::remove_all(root);
    std::printf("%s\n",
                g_fail ? "FAIL"
                       : "PASS — captures from both sides of a version bump read into one table");
    return g_fail;
}
