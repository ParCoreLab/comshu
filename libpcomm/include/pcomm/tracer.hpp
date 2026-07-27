#pragma once
// L4 — Tracer: the in-process orchestrator (controlled singleton; lifetime
// tied to LD_PRELOAD ctor/dtor, but injectable for tests). Owns the recorder,
// the provider registry, and the calibrator. Providers talk to it, never to
// each other. Skeleton: recorder + sink wiring and start/stop; the dlopen
// ProviderRegistry and Calibrator land with the first real provider.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <pthread.h>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <string>

#include "pcomm/core/records.hpp"
#include "pcomm/helpers/clock.hpp"
#include "pcomm/rec/recorder.hpp"
#include "pcomm/rec/sink.hpp"
#include "pcomm/rec/thread_buffer_registry.hpp"

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
    // Defined in src/engine.cpp, NOT here: a function-local static in a header
    // gives every provider .so its own copy with weak linkage, which is exactly
    // how two providers came to share a Tracer by accident and destroy each
    // other's Recorder (7a5b55a). One definition site, one instance.
    static Tracer& instance();

    // Bringing the engine up is *attach-if-running*, not re-init. Two providers
    // preloaded together (e.g. ib + gpumem) each run their own ELF ctor and each
    // call start(); the engine is one singleton, so the second call used to
    // assign over m_recorder and destroy the first provider's live Recorder
    // mid-flight — SIGSEGV inside ~ThreadLocalBuffer during _dl_init. The second
    // provider now simply joins the running capture, which is also what we want
    // semantically: one process, one recorder, one timeline.
    void start(const Config& cfg) {
        std::lock_guard<std::mutex> lk(m_lifecycle);
        ++m_active;
        if (m_recorder) {
            // Providers all build their Config from the same env, so a mismatch
            // means someone is being started differently — first one wins, but
            // say so rather than silently splitting the capture.
            if (cfg.output_dir != m_config.output_dir || cfg.rank != m_config.rank)
                std::fprintf(stderr,
                             "[pcomm] provider requested %s/rank-%d but capture is "
                             "already running as %s/rank-%d; joining the latter\n",
                             cfg.output_dir.c_str(), cfg.rank,
                             m_config.output_dir.c_str(), m_config.rank);
            return;
        }
        m_config = cfg;
        auto dir = m_config.output_dir / ("rank-" + std::to_string(m_config.rank));
        m_sink = std::make_unique<rec::FileSink>(dir);
        m_recorder = std::make_unique<rec::Recorder<>>(*m_sink);
        install_fork_handler();
        // Backstop: with refcounted stop(), a provider that never stops (a
        // vendor teardown path that skips our dtor) would otherwise hold the
        // count above zero and the capture would never be flushed at all.
        std::atexit([] { instance().finalize(); });
        emit_anchor();  // start-of-run clock anchor (UNIFICATION §4)
    }

    // Symmetric: only the last provider to leave finalizes the capture, so one
    // provider unloading early can't flush and seal the streams the other is
    // still writing to (and we don't emit a second end-of-run anchor).
    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_lifecycle);
            if (!m_recorder) return;
            if (m_active > 0 && --m_active > 0) return;
        }
        finalize();
    }

    // The actual end-of-capture work, run exactly once however we get here
    // (last stop(), or the atexit backstop).
    void finalize() {
        {
            std::lock_guard<std::mutex> lk(m_lifecycle);
            if (!m_recorder || m_finalized) return;
            m_finalized = true;
        }
        emit_anchor();  // end-of-run anchor: reader fits offset + skew
        // Flush every thread's buffers, not just this one's (cross-thread
        // flush): multi-threaded targets (e.g. a CQ-poll thread) record on
        // threads that never call stop().
        rec::ThreadBufferRegistry::instance().drain_all();
        dump_intern_sidecars();
        m_sink->flush();
    }

    rec::Recorder<>& recorder() { return *m_recorder; }
    // Deliberately lock-free: the arena drain path calls this, and drain can be
    // reached from inside start() (emit_anchor) which already holds m_lifecycle.
    bool has_recorder() const { return m_recorder != nullptr; }
    const Config& config() const { return m_config; }

private:
    Tracer() = default;

    // Register the fork child-handler once (idempotent across start() calls,
    // including the re-init start() the child handler itself performs).
    void install_fork_handler() {
        static std::once_flag once;
        std::call_once(once, [] { pthread_atfork(nullptr, nullptr, &Tracer::atfork_child); });
    }

    // Runs in the child right after fork(). The child shares (COW) the parent's
    // sink FILE*s and buffered records; letting the parent's sink/recorder
    // destruct here would flush+fclose those shared fds and corrupt the
    // parent's files. So we *release* (leak) them instead of destroying, drop
    // the tracked buffers unflushed, and re-init into a child-private dir so
    // the child records independently. The parent process is untouched (child
    // handler only). Not async-signal-safe (allocates) — matches SigmaTrace;
    // safe for the common fork-then-record / fork-then-exec patterns, not for
    // forking while another thread holds malloc/intern locks.
    static void atfork_child() {
        Tracer& t = instance();
        if (!t.m_recorder) return;  // wasn't tracing
        (void)t.m_sink.release();      // leak parent's sink: do NOT flush/fclose
        (void)t.m_recorder.release();  // leak parent's recorder: do NOT drain
        rec::ThreadBufferRegistry::instance().reset_for_fork();
        // Child rank <- pid: guarantees a distinct rank-<pid> dir from the
        // parent. A launcher that re-execs overwrites this with the real rank
        // on its next start(). The fresh Recorder has a new address, so the
        // per-thread buffer_store rebuilds against the child's sink.
        Config cfg = t.m_config;
        cfg.rank = static_cast<int>(::getpid());
        // The providers are still attached in the child, so re-init must not
        // count as an extra start() — otherwise the refcount never reaches zero
        // and the child's capture is never flushed.
        const int attached = t.m_active;
        t.start(cfg);
        t.m_active = attached;
    }

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
    // How many providers have start()ed and not yet stop()ped. Guards the
    // attach/finalize decisions above; ctors normally run single-threaded under
    // _dl_init, but stop() can arrive from a vendor exit callback on any thread.
    int m_active{0};
    bool m_finalized{false};
    std::mutex m_lifecycle;
};

inline Config Config::from_env() {
    Config cfg;
    if (const char* v = std::getenv("PCOMM_OUTPUT_DIR")) cfg.output_dir = v;
    if (const char* v = std::getenv("PCOMM_VERBOSE")) cfg.verbose = *v == '1';
    // Rank guess (SigmaTrace's RankDetector, minimal port): explicit override,
    // then launcher-provided env, then pid so concurrent unranked processes
    // never clobber each other's streams.
    cfg.rank = -1;
    for (const char* var : {"PCOMM_RANK", "OMPI_COMM_WORLD_RANK", "PMIX_RANK",
                            "PMI_RANK", "SLURM_PROCID", "NVSHMEM_MY_PE"}) {
        if (const char* v = std::getenv(var)) {
            cfg.rank = std::atoi(v);
            break;
        }
    }
    if (cfg.rank < 0) cfg.rank = static_cast<int>(getpid());
    return cfg;
}

}  // namespace pcomm
