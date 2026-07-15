// Smoke test for the L0/L1 core: record all four archetypes through a
// Recorder into a FileSink, plus the attribution pipeline over synthetic
// accesses. No GPU, no vendor libs.

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "pcomm/attr/pipeline.hpp"
#include "pcomm/core/records.hpp"
#include "pcomm/rec/recorder.hpp"

using namespace pcomm;
using core::records::GpuAccess;

namespace {

class CountingSink final : public rec::ISink {
public:
    void submit(const rec::BufferView& v) override {
        total_records += v.count;
        total_bytes += v.bytes;
    }
    void flush() override {}
    std::size_t total_records{0};
    std::size_t total_bytes{0};
};

class RemoteFilter final : public attr::IFilter {
public:
    bool keep(const attr::RawAccess& a) const override {
        return a.run_device != a.mem_device;
    }
};

}  // namespace

int main() {
    namespace fs = std::filesystem;

    // --- interning: same value -> same handle, distinct values -> distinct ---
    {
        rec::InternTable t;
        auto a = t.intern("foo.cu"), b = t.intern("bar.cu"), c = t.intern("foo.cu");
        assert(a == c && a != b && a != core::kNullHandle);
        assert(t.intern(std::uint64_t{0x1f80}) == t.intern(std::uint64_t{0x1f80}));
        assert(t.size() == 3);
    }

    // --- recorder: all four archetypes, buffer drain accounting ---------------
    {
        CountingSink sink;
        rec::Recorder<> rec(sink);

        auto pc = rec.intern(rec::InternDomain::Pc, std::uint64_t{0x1f80});
        auto file = rec.intern(rec::InternDomain::String, "stencil.cu");

        constexpr std::size_t n = 10'000;  // > buffer capacity: forces drains
        for (std::size_t i = 0; i < n; ++i) {
            auto& buf = rec.buffer<GpuAccess>();
            GpuAccess* slot = buf.slot(rec.now_instant());
            slot->pc = pc;
            slot->address = 0x7000'0000 + i * 4;
            slot->size = 4;
            slot->op = core::MemOp::Store;
            slot->run_device = 0;
            slot->mem_device = 2;
            buf.commit();
        }
        rec.record(core::Temporal::none(), core::records::SourceLine{{}, {}, pc, file, 42});
        rec.record(core::Temporal::none(),
                   core::records::MemObject{{}, {}, 1, file, {0x7000'0000, 1 << 20}, 2});
        rec.buffer<GpuAccess>().drain();
        rec.buffer<core::records::SourceLine>().drain();
        rec.buffer<core::records::MemObject>().drain();
        assert(sink.total_records == n + 2);
    }

    // --- attribution pipeline: filter drops local accesses --------------------
    {
        CountingSink sink;
        rec::Recorder<> rec(sink);
        attr::AttributionPipeline pipe(rec);
        pipe.add_filter(std::make_unique<RemoteFilter>());

        std::vector<attr::RawAccess> accesses(100);
        for (std::size_t i = 0; i < accesses.size(); ++i) {
            accesses[i].pc = 0xabc;
            accesses[i].run_device = 0;
            accesses[i].mem_device = (i % 2) ? 1 : 0;  // half remote, half local
        }
        pipe.process(accesses);
        rec.buffer<GpuAccess>().drain();
        assert(sink.total_records == 50);
    }

    // --- file sink: homogeneous per-type streams appear on disk ---------------
    {
        auto dir = fs::temp_directory_path() / "pcomm-core-smoke";
        fs::remove_all(dir);
        {
            rec::FileSink sink(dir);
            rec::Recorder<> rec(sink);
            for (int i = 0; i < 100; ++i) rec.record(GpuAccess{{}, {}, 1, 0, 0x100, 4});
            rec.buffer<GpuAccess>().drain();
        }
        auto stream = dir / "700.pcs";
        assert(fs::exists(stream));
        assert(fs::file_size(stream) == 100 * sizeof(rec::Stamped<GpuAccess>));
        fs::remove_all(dir);
    }

    std::puts("core_smoke: OK");
    return 0;
}
