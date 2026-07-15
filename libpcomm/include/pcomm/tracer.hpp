#pragma once
// L4 — Tracer: the in-process orchestrator (controlled singleton; lifetime
// tied to LD_PRELOAD ctor/dtor, but injectable for tests). Owns the recorder,
// the provider registry, and the calibrator. Providers talk to it, never to
// each other. Skeleton: recorder + sink wiring and start/stop; the dlopen
// ProviderRegistry and Calibrator land with the first real provider.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "pcomm/core/records.hpp"
#include "pcomm/helpers/clock.hpp"
#include "pcomm/rec/recorder.hpp"
#include "pcomm/rec/sink.hpp"

namespace pcomm {

struct Config {
    std::filesystem::path output_dir{"pcomm-out"};
    int  rank{0};
    bool verbose{false};

    // Builder: env (PCOMM_*) -> immutable run config. Set by the launcher.
    static Config from_env();
};

class Tracer {
public:
    // Deliberately leaked (never destroyed): backends invoke stop() from
    // exit-time vendor callbacks (e.g. NVBit's atexit handler) that can run
    // after C++ static destructors. All buffered data is flushed by stop();
    // the leak is process-exit-only.
    static Tracer& instance() {
        static Tracer* t = new Tracer();
        return *t;
    }

    void start(const Config& cfg) {
        m_config = cfg;
        auto dir = m_config.output_dir / ("rank-" + std::to_string(m_config.rank));
        m_sink = std::make_unique<rec::FileSink>(dir);
        m_recorder = std::make_unique<rec::Recorder<>>(*m_sink);
        emit_anchor();  // start-of-run clock anchor (UNIFICATION §4)
    }

    void stop() {
        if (!m_recorder) return;
        emit_anchor();  // end-of-run anchor: reader fits offset + skew
        m_recorder->buffer<core::records::ClockAnchor>().drain();
        dump_intern_sidecars();
        m_sink->flush();
    }

    rec::Recorder<>& recorder() { return *m_recorder; }
    const Config& config() const { return m_config; }

private:
    Tracer() = default;

    void emit_anchor() {
        auto s = helpers::sample_anchor();
        m_recorder->record(core::Temporal::none(),
                           core::records::ClockAnchor{{}, {}, /*domain=*/0,
                                                      s.local_ticks, s.wall_ns, s.mono_ns});
    }

    // Intern dictionaries resolve handles in the streams; written as TSV
    // sidecars at stop (skeleton form of the sidecar streams in UNIFICATION
    // §5.1). String domains: handle\tvalue. Numeric domains: handle\tkey.
    void dump_intern_sidecars() {
        static constexpr std::string_view names[] = {"pc", "pointer", "string",
                                                     "kernel", "type"};
        for (std::size_t d = 0; d < rec::kInternDomainCount; ++d) {
            auto& table = m_recorder->table(static_cast<rec::InternDomain>(d));
            if (table.size() == 0) continue;
            auto path = m_sink->dir() / ("intern-" + std::string(names[d]) + ".tsv");
            std::FILE* f = std::fopen(path.c_str(), "w");
            if (!f) continue;
            core::Handle h = 0;
            for (const auto& s : table.strings())
                std::fprintf(f, "%u\t%s\n", ++h, s.c_str());
            for (const auto& [key, handle] : table.keys())
                std::fprintf(f, "%u\t0x%llx\n", handle,
                             static_cast<unsigned long long>(key));
            std::fclose(f);
        }
    }

    Config m_config{};
    std::unique_ptr<rec::FileSink> m_sink;
    std::unique_ptr<rec::Recorder<>> m_recorder;
};

inline Config Config::from_env() {
    Config cfg;
    if (const char* v = std::getenv("PCOMM_OUTPUT_DIR")) cfg.output_dir = v;
    if (const char* v = std::getenv("PCOMM_RANK")) cfg.rank = std::atoi(v);
    if (const char* v = std::getenv("PCOMM_VERBOSE")) cfg.verbose = *v == '1';
    return cfg;
}

}  // namespace pcomm
