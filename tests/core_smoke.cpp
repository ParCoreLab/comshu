// Smoke test for the L0/L1 core: record all four archetypes through a
// Recorder into a FileSink, plus the attribution pipeline over synthetic
// accesses. No GPU, no vendor libs.

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "pcomm/attr/pipeline.hpp"
#include "pcomm/core/records.hpp"
#include "pcomm/rec/recorder.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/tracer.hpp"

using namespace pcomm;
using core::records::GpuAccess;

namespace {

class CountingSink final : public rec::ISink {
public:
    void submit(const rec::BufferView& v) override {
        total_records += v.count;
        total_bytes += v.bytes;
    }
    void        flush() override {}
    std::size_t total_records{0};
    std::size_t total_bytes{0};
};

class RemoteFilter final : public attr::IFilter {
public:
    bool keep(const attr::RawAccess& a) const override { return a.run_device != a.mem_device; }
};

}  // namespace

int main() {
    namespace fs = std::filesystem;

    // --- interning: same value -> same handle, distinct values -> distinct ---
    {
        rec::InternTable t;
        auto             a = t.intern("foo.cu"), b = t.intern("bar.cu"), c = t.intern("foo.cu");
        assert(a == c && a != b && a != core::kNullHandle);
        assert(t.intern(std::uint64_t{0x1f80}) == t.intern(std::uint64_t{0x1f80}));
        assert(t.size() == 3);
    }

    // --- buffers: slot/commit/drain accounting against a sink ----------------
    // This exercises ThreadLocalBuffer, which is what `Recorder::buffer<R>()`
    // used to hand out. Since 4dd6a69 the Recorder writes into engine-owned
    // arenas instead, so a sink a test constructs itself never sees a record —
    // the buffer is the layer where a local ISink still means something.
    {
        CountingSink     sink;
        rec::InternTable strings;
        auto             pc = strings.intern(std::uint64_t{0x1f80});
        auto             file = strings.intern("stencil.cu");

        constexpr std::size_t             n = 10'000;  // > buffer capacity: forces drains
        rec::ThreadLocalBuffer<GpuAccess> accesses(sink);
        for (std::size_t i = 0; i < n; ++i) {
            GpuAccess* slot = accesses.slot(core::Temporal::none());
            slot->pc = pc;
            slot->address = 0x7000'0000 + i * 4;
            slot->size = 4;
            slot->op = core::MemOp::Store;
            slot->run_device = 0;
            slot->mem_device = 2;
            accesses.commit();
        }
        rec::ThreadLocalBuffer<core::records::SourceLine> lines(sink);
        *lines.slot(core::Temporal::none()) = core::records::SourceLine{{}, {}, pc, file, 42};
        lines.commit();
        rec::ThreadLocalBuffer<core::records::MemObject> objects(sink);
        *objects.slot(core::Temporal::none()) =
            core::records::MemObject{{}, {}, 1, file, {0x7000'0000, 1 << 20}, 2};
        objects.commit();

        accesses.drain();
        lines.drain();
        objects.drain();
        assert(sink.total_records == n + 2);
    }

    // --- one capture, two checks: the pipeline's filter, and the file sink -----
    //
    // These were two blocks with a Tracer capture each, and that was a latent
    // bug in the test rather than a choice. Tracer is a process singleton whose
    // start() is attach-if-running (deliberately — two preloaded providers must
    // join one capture, see tracer.hpp), and finalize() does not clear
    // m_recorder. So a second start() with a different output_dir *joins the
    // first capture* and prints "joining the latter"; the second block's
    // directory was never created and its `assert(fs::exists(stream))` was
    // false. Nobody saw it because NDEBUG had compiled every assert away.
    //
    // One capture, therefore, with both checks accounted against it. (Whether
    // the Tracer *should* support a restart after finalize is a product
    // question, noted in things-to-circle-back-to.md; in production finalize
    // runs once at exit, so it has never mattered outside this test.)
    {
        auto dir = fs::temp_directory_path() / "pcomm-core-smoke";
        fs::remove_all(dir);
        Config cfg;
        cfg.output_dir = dir;
        cfg.rank = 0;
        Tracer::instance().start(cfg);

        // (a) AttributionPipeline holds a Recorder, so its output goes through
        //     the engine to the engine's sink; count where records land.
        attr::AttributionPipeline pipe(Tracer::instance().recorder());
        pipe.add_filter(std::make_unique<RemoteFilter>());

        std::vector<attr::RawAccess> accesses(100);
        for (std::size_t i = 0; i < accesses.size(); ++i) {
            accesses[i].pc = 0xabc;
            accesses[i].run_device = 0;
            accesses[i].mem_device = (i % 2) ? 1 : 0;  // half remote, half local
            accesses[i].ticks_lo = 1000 + i;           // as a producer bounds it
            accesses[i].ticks_hi = 2000 + i;
        }
        // One kept access (odd index => remote) with no bound at all, standing
        // in for a producer that cannot supply one — an NVBit launch that aged
        // out of the ring, or a CUDA-graph node launch we do not intercept.
        accesses[1].ticks_lo = 0;
        accesses[1].ticks_hi = 0;
        pipe.process(accesses);  // filter keeps the 50 remote ones

        // (b) the same stream, written directly through the Recorder.
        auto& recorder = Tracer::instance().recorder();
        for (int i = 0; i < 100; ++i)
            recorder.record(GpuAccess{{}, {}, 1, 0, 0x100, 4});

        Tracer::instance().finalize();

        auto stream = dir / "rank-0" / "700.pcs";  // Tracer::start makes rank-N/
        assert(fs::exists(stream));
        // 50 survived the filter + 100 direct. A filter that dropped nothing
        // would give 200, one that dropped everything 100 — the count still
        // pins the behaviour even though the two writers share a stream.
        assert(fs::file_size(stream) ==
                   sizeof(rec::StreamHeader) + 150 * sizeof(rec::Stamped<GpuAccess>) &&
               "filter should drop the 50 local accesses, and both writers land here");

        // The file says what it is. Check it says the right thing, in the right
        // place — the header is the first 32 bytes of every capture we ship.
        rec::StreamHeader h{};
        std::ifstream     in(stream, std::ios::binary);
        in.read(reinterpret_cast<char*>(&h), sizeof h);
        assert(h.magic == rec::kStreamMagic);
        assert(h.format_version == rec::kStreamFormatVersion);
        assert(h.type_id == core::RecordTrait<GpuAccess>::type_id);
        assert(h.kind == core::RecordKind::Event);
        assert(h.stride == sizeof(rec::Stamped<GpuAccess>));
        assert(h.schema_hash == rec::kSchemaHash);
        assert(rec::stream_header_error(h, core::RecordTrait<GpuAccess>::type_id,
                                        sizeof(rec::Stamped<GpuAccess>),
                                        core::RecordTrait<GpuAccess>::kind) == nullptr);

        // (c) what time do those records claim? The two writers must differ, and
        //     the difference is the point:
        //       - the pipeline carries the *producer's* bound  -> Window
        //         (or None when the producer had none — never a made-up instant)
        //       - Recorder::record(r) with no Temporal          -> Instant
        //     A GPU access dated with the drain thread's own clock is a false
        //     precision claim; that is what this pins. Nothing else in the tree
        //     does — no suite golden queries time, so a regression here turns no
        //     board red. See plans/PLAN-20260803-1021-gpumem-window-*.md.
        {
            std::ifstream ts(stream, std::ios::binary);
            ts.seekg(sizeof(rec::StreamHeader));
            std::size_t             windows = 0, timeless = 0, instants = 0;
            rec::Stamped<GpuAccess> s{};
            while (ts.read(reinterpret_cast<char*>(&s), sizeof s)) {
                switch (s.temporal.kind) {
                    case core::TimeKind::Window:
                        ++windows;
                        assert(s.temporal.a.ticks >= 1000 && s.temporal.a.ticks < 1100 &&
                               "lower bound must be the producer's, not a host clock");
                        assert(s.temporal.b.ticks > s.temporal.a.ticks &&
                               "a window must not close before it opens");
                        break;
                    case core::TimeKind::None:
                        ++timeless;
                        break;
                    case core::TimeKind::Instant:
                        ++instants;
                        break;
                    default:
                        assert(false && "unexpected TimeKind in a gpu_access stream");
                }
            }
            assert(windows == 49 && "the 49 bounded accesses the filter kept");
            assert(timeless == 1 && "the unbounded one: timeless, not invented");
            assert(instants == 100 && "the direct Recorder::record path is unchanged");
        }
        fs::remove_all(dir);
    }

    std::puts("core_smoke: OK");
    return 0;
}
