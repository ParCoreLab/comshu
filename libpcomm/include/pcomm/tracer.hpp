#pragma once
// L4 — Tracer: the in-process orchestrator (controlled singleton; lifetime
// tied to LD_PRELOAD ctor/dtor, but injectable for tests). Owns the recorder,
// the provider registry, and the calibrator. Providers talk to it, never to
// each other. Skeleton: recorder + sink wiring and start/stop; the dlopen
// ProviderRegistry and Calibrator land with the first real provider.

#include <csignal>
#include <signal.h>
#include <cstdio>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <pthread.h>
#include <unistd.h>
#include <memory>
#include <vector>
#include <mutex>
#include <string>

#include "pcomm/core/records.hpp"
#include "pcomm/helpers/clock.hpp"
#include "pcomm/rec/recorder.hpp"
#include "pcomm/rec/sink.hpp"
#include "pcomm/rec/thread_buffer_registry.hpp"

// Fatal-signal handlers, defined in src/engine.cpp. Declared here because
// start() is where a capture begins and so is the only place that knows a
// handler is wanted; defined THERE because a process-wide signal disposition
// belongs to no single provider and two providers must not install two
// handlers — engine_abi.h's argument for owning ibv_destroy_qp, applied again.
void pcomm_install_signal_handlers();

namespace pcomm {

struct Config {
    std::filesystem::path output_dir{"pcomm-out"};
    int                   rank{0};
    bool                  verbose{false};

    // Builder: env (PCOMM_*) -> immutable run config. Set by the launcher.
    static Config from_env();
};

// Write the Invocation record (532) unless it has already been written.
//
// Defined in src/engine.cpp, beside the provider registry and the build-id
// header it reads, and declared here because Tracer::finalize() is one of its
// two call sites. Idempotent: whichever site reaches it first wins.
void maybe_emit_invocation();

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
                             cfg.output_dir.c_str(), cfg.rank, m_config.output_dir.c_str(),
                             m_config.rank);
            return;
        }
        m_config = cfg;
        auto dir = m_config.output_dir / ("rank-" + std::to_string(m_config.rank));
        m_sink = std::make_unique<rec::FileSink>(dir);
        m_recorder = std::make_unique<rec::Recorder<>>(*m_sink);
        install_fork_handler();
        ::pcomm_install_signal_handlers();
        // Backstop: with refcounted stop(), a provider that never stops (a
        // vendor teardown path that skips our dtor) would otherwise hold the
        // count above zero and the capture would never be flushed at all.
        std::atexit([] { instance().finalize(); });
        emit_anchor();         // start-of-run clock anchor (UNIFICATION §4)
        emit_node_identity();  // which machine this rank is on
        // ...AND PUT THEM ON DISK NOW, rather than leaving them in a thread
        // buffer that only finalize() will ever drain.
        //
        // Nothing flushes on an abnormal exit — there is no signal handler
        // anywhere in the engine — so a process killed by a signal keeps only
        // what happened to have been flushed already. That fails in an INVERTED
        // way: a high-volume stream survives because it filled its buffers and
        // drained incrementally, while a stream of one or two rows never did.
        // Measured on Quantum ESPRESSO (tests/hpc/apps/03_quantumespresso),
        // whose pw.x reaches JOB DONE and then faults in its own teardown:
        // 18,700,773 ib_post_send rows survived, node_identity covered 2 of 4
        // ranks, and 84% of sends therefore landed on dst_rank = -1. The broken
        // capture was the BIGGER one.
        //
        // These two records are the cheapest possible defence and the most
        // valuable rows to defend: they are written once per rank, they are what
        // every rank-space and time-space join is anchored on, and both are pure
        // numbers — NodeIdentity carries a hash and ClockAnchor carries ticks,
        // so neither needs the intern dictionary that only finalize() emits. A
        // record flushed early whose Handles are undefined until teardown would
        // be no better than a lost one; these have none.
        //
        // This does NOT make the capture crash-safe. Everything recorded after
        // this point is still lost on a signal, which is todos.txt item 15.
        rec::ThreadBufferRegistry::instance().drain_all();
        m_sink->flush();
    }

    // Symmetric: only the last provider to leave finalizes the capture, so one
    // provider unloading early can't flush and seal the streams the other is
    // still writing to (and we don't emit a second end-of-run anchor).
    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_lifecycle);
            if (!m_recorder)
                return;
            if (m_active > 0 && --m_active > 0)
                return;
        }
        finalize();
    }

    // The actual end-of-capture work, run exactly once however we get here
    // (last stop(), or the atexit backstop).
    void finalize() {
        {
            std::lock_guard<std::mutex> lk(m_lifecycle);
            if (!m_recorder || m_finalized)
                return;
            m_finalized = true;
        }
        emit_anchor();  // end-of-run anchor: reader fits offset + skew
        // Flush every thread's buffers, not just this one's (cross-thread
        // flush): multi-threaded targets (e.g. a CQ-poll thread) record on
        // threads that never call stop().
        rec::ThreadBufferRegistry::instance().drain_all();
        // NOT emit_gpu_devices() here: by this point the CUDA driver has
        // returned CUDA_ERROR_DEINITIALIZED and the enumeration yields nothing.
        // It runs from the arena-flush path instead — see maybe_emit_gpu_devices.
        //
        // INVOCATION, HOWEVER, IS EMITTED HERE, and the asymmetry is the point.
        // It rides the arena-flush path too, which needs a record buffer to
        // FILL — so a short capture never reached it. Measured on MN5 job
        // 45344306: hubspoke_ib (41,721 sends) and nvshmem_multinode carried
        // their invocation rows, while ucx_deep, ib_pingpong (40 sends) and
        // ib_send_bw (1,000) carried NONE.
        //
        // GpuDevice's boundary comment argues that a capture too small to fill
        // a buffer "has nothing to attribute to a GPU anyway". That is true of
        // GpuDevice and false of this record: a tiny capture still has an argv,
        // a build id and a .so that answered, and those are worth MORE on a
        // small odd capture, not less. Case 21 — where "which binary answered"
        // cost two days this session — was one of the empty ones.
        //
        // Safe here where GpuDevice is not, because nothing this reads goes
        // away at teardown: /proc/self/cmdline, environ, /proc/self/maps and the
        // declared-provider list are all still readable. It must run BEFORE
        // emit_intern_records(), since every field it writes is an interned
        // string; the gate makes it a no-op when the flush path got there first.
        maybe_emit_invocation();
        // Emitted after the application's records, then drained themselves: the
        // dictionary describes handles that by now all exist.
        emit_intern_records();
        rec::ThreadBufferRegistry::instance().drain_all();
        m_sink->flush();
        // LAST, and after that flush rather than before it. Everything this rank
        // will ever write is now on disk, so the sink's loss counters are final
        // and the attestation describes the whole capture instead of a prefix of
        // it. Its own stream is the one thing it cannot describe — see the
        // record's comment.
        emit_capture_status();
        rec::ThreadBufferRegistry::instance().drain_all();
        m_sink->flush();
        report_losses();
    }

    // Best-effort flush from a fatal-signal handler, in this order and for the
    // reasons the handler itself records (src/engine.cpp, "dying gracefully").
    // Public because that handler lives there rather than here: a process-wide
    // signal disposition belongs to no single provider, which is the same
    // argument engine_abi.h makes for owning ibv_destroy_qp.
    void flush_from_signal() noexcept {
        // Lock-free reads on purpose: m_lifecycle may be held by the thread that
        // just faulted, and taking it here is the deadlock guard 3 exists to
        // avoid. Both pointers are written once under the lock in start() and
        // never reassigned while a capture is live.
        if (!m_recorder || !m_sink)
            return;
        if (m_finalized)
            return;  // a normal teardown already wrote everything

        // (a) the safe half — move what is buffered to the sink and make it
        //     durable. If anything below faults, this survives.
        //
        //     NOT COVERED BY A MUTATION, and worth saying so rather than
        //     implying otherwise: deleting this flush leaves the test green,
        //     because on a clean fault (b) succeeds and the flush at the end
        //     writes everything anyway. Its value appears only when (b) itself
        //     faults, which no mutation reaches without injecting a fault into
        //     emit_intern_records. It stays because the ordering is the point of
        //     guard 1 and the flush costs one fwrite on a dying process.
        rec::ThreadBufferRegistry::instance().try_drain_all();
        m_sink->flush();

        // (b) the risky half — the dictionary that gives every Handle meaning.
        //     Worth attempting because without it a recovered record carrying
        //     handles says nothing; safe to attempt only because (a) is already
        //     on disk and the recursion guard turns a fault here into an
        //     ordinary crash rather than a loop.
        emit_intern_records();
        rec::ThreadBufferRegistry::instance().try_drain_all();
        m_sink->flush();
    }

    rec::Recorder<>& recorder() { return *m_recorder; }
    // Deliberately lock-free: the arena drain path calls this, and drain can be
    // reached from inside start() (emit_anchor) which already holds m_lifecycle.
    bool          has_recorder() const { return m_recorder != nullptr; }
    const Config& config() const { return m_config; }

    // Public for engine.cpp's arena-flush path to call, exactly as
    // flush_from_signal is public for its signal handler: the call site is a
    // C ABI function that cannot be a member, and the alternative is a friend
    // declaration naming a free function with C linkage.
    // DURING THE RUN, and neither at start nor at stop — both were tried and
    // both are structurally wrong.
    //
    // Start is too early: the Tracer is constructed from the LD_PRELOAD ctor,
    // before main, and an application that loads CUDA later has no driver to
    // ask yet. Stop is too late, and it fails in the most misleading way
    // available. Measured on job 45302619, from the traced process:
    //
    //   [after cuda use]  cuInit=0  cuDeviceGetCount=0  n=1
    //                     attr(bus)=27 (0x1B, matching nvidia-smi)  "NVIDIA H100"
    //   [static dtor]     cuInit=4  cuDeviceGetCount=4  n=-1
    //
    // 4 is CUDA_ERROR_DEINITIALIZED. libcuda stays MAPPED through teardown — so
    // dlopen(RTLD_NOLOAD) keeps returning a good handle and every symbol keeps
    // resolving — while the driver has already refused to answer. Two earlier
    // attempts died on exactly that: the handle looked healthy and the table
    // was empty.
    //
    // So it is called from the arena-flush path, which happens whenever a
    // record buffer fills, i.e. repeatedly and while the application is alive.
    // One relaxed atomic load on a path that is already doing I/O.
    //
    // KNOWN BOUNDARY, stated rather than discovered later: a capture too short
    // to fill a single buffer never flushes in-run and will carry no
    // gpu_device rows. Every workload this project traces produces millions of
    // records; a capture that produces fewer than one buffer's worth has
    // nothing to attribute to a GPU anyway.
    //
    // Retried until it succeeds rather than attempted once, because "CUDA is
    // not loaded yet" and "there is no CUDA here" are indistinguishable at any
    // single moment and only the first of them changes.
    void maybe_emit_gpu_devices() {
        if (m_gpu_devices_done.load(std::memory_order_relaxed))
            return;
        if (!m_recorder)
            return;
        emit_gpu_devices();
    }



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
    //
    // NB the arenas the child records into are the same objects the parent was
    // using — they are keyed by type_id in a thread-local map, and a cached
    // pcomm_arena* in Recorder::append survives the fork. That is fine and
    // deliberate: Arena::drain() resolves the sink from the Tracer at drain
    // time, so after the start() below they already write to the child's
    // directory. What they need is their *content* cleared and their
    // *registration* restored — see pcomm_engine_reset_arenas_for_fork.
    static void atfork_child() {
        Tracer& t = instance();
        if (!t.m_recorder)
            return;                    // wasn't tracing
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
        // After start(), so the arenas resolve to the child's sink and the
        // registry mutex reset_for_fork() placement-reset is usable again.
        // Without this the child's records land in arenas nobody drains: the
        // parent wrote 130/130 and the child 0 of 50 (fork_smoke).
        pcomm_engine_reset_arenas_for_fork();
    }

    // Say it, loudly, on the way out.
    //
    // Until 2026-09-03 the sink ignored every write result, so a capture that
    // lost whole streams to a filesystem quota exited 0 and looked fine. Three
    // MN5 suite cases went red pointing at InfiniBand and UCX behaviour; the
    // fault was `gpfs_home` at 83.99 GB against an 80 GB quota. See
    // plans/PLAN-20260903-0100.
    //
    // stderr and not a record, on purpose. The one condition this reports is
    // "writes to this directory are failing", and a record describing it would
    // be written through the same sink that just failed. The reader has its own
    // half of this — a stream with no header is detectable from the file — and
    // the two together mean neither surface has to be trusted alone.
    //
    // PCOMM_STRICT_WRITES=1 turns it fatal. Not the default: a capture that lost
    // a stream is still worth analysing, and killing a 4-node job at teardown
    // over it would destroy the rest of the evidence. But a suite that wants a
    // board it can trust should set it, which is exactly what the HPC runner now
    // does.
    // The durable half of report_losses. That one writes to stderr, which is the
    // right place for a human running a job and the wrong place for the Parquet
    // that outlives it; this one puts the same facts where `pcomm link` can read
    // them. See core/records.hpp CaptureStatus for the measurement that made the
    // case, and for why absence carries more information than any field here.
    void emit_capture_status() {
        if (!m_recorder || !m_sink)
            return;
        const auto    losses = m_sink->losses();
        std::uint64_t records = 0, bytes = 0;
        bool          header_lost = false, never_opened = false;
        for (const auto& l : losses) {
            records += l.lost_records;
            bytes += l.lost_bytes;
            header_lost |= l.header_lost;
            never_opened |= l.never_opened;
        }
        m_recorder->record(
            core::Temporal::none(),
            core::records::CaptureStatus{
                {},
                {},
                records,
                bytes,
                // +1 for THIS record's own stream, which the sink has not
                // created yet — the count is taken before the record is drained.
                // Without it the reader's file count is one higher than the
                // attested one on every healthy capture, and the one comparison
                // that catches a stream deleted AFTER the run would either fire
                // on everything or have to guess. Counting it here keeps that
                // arithmetic in the place that knows why it is needed.
                static_cast<std::uint32_t>(m_sink->open_streams() + 1),
                static_cast<std::uint32_t>(losses.size()),
                static_cast<std::uint8_t>(m_sink->directory_failed() ? 1 : 0),
                static_cast<std::uint8_t>(header_lost ? 1 : 0),
                static_cast<std::uint8_t>(never_opened ? 1 : 0),
                0,
                0});
    }

    void report_losses() {
        if (!m_sink)
            return;
        const bool dir_failed = m_sink->directory_failed();
        const auto losses = m_sink->losses();
        if (!dir_failed && losses.empty())
            return;

        std::fprintf(stderr, "pcomm: CAPTURE INCOMPLETE in %s\n", m_sink->dir().c_str());
        if (dir_failed)
            std::fprintf(stderr, "  the output directory could not be created — nothing was written\n");
        std::uint64_t records = 0;
        for (const auto& l : losses) {
            records += l.lost_records;
            std::fprintf(stderr, "  stream %u: %llu record(s), %llu byte(s) lost%s%s\n", l.type_id,
                         static_cast<unsigned long long>(l.lost_records),
                         static_cast<unsigned long long>(l.lost_bytes),
                         l.header_lost ? " — HEADER LOST, stream is unreadable" : "",
                         l.never_opened ? " — file could never be opened" : "");
        }
        std::fprintf(stderr,
                     "  This capture is missing data. Check free space and quota on that "
                     "filesystem before trusting any table derived from it.\n");

        const char* strict = std::getenv("PCOMM_STRICT_WRITES");
        if (strict && *strict && *strict != '0') {
            std::fprintf(stderr, "  PCOMM_STRICT_WRITES is set — failing the run (%llu records lost)\n",
                         static_cast<unsigned long long>(records));
            std::_Exit(70);  // EX_SOFTWARE; _Exit so no atexit handler re-enters finalize
        }
    }

    void emit_anchor() {
        auto s = helpers::sample_anchor();
        m_recorder->record(
            core::Temporal::none(),
            core::records::ClockAnchor{{}, {}, /*domain=*/0, s.local_ticks, s.wall_ns, s.mono_ns});
    }

    // Which machine this rank is on. Once per rank, at start, beside the anchor
    // — both are facts about the run rather than about anything the application
    // did, and both are useless if written later than the records that need
    // them.
    //
    // A hash, not the hostname: the id only has to be comparable between ranks
    // of one job. `gethostname` failing is recorded as 0 rather than skipped,
    // so "this capture has no node identity" and "this rank could not read its
    // hostname" stay distinguishable — the reader treats 0 as unknown and falls
    // back, instead of silently grouping every failed rank onto one node.
    void emit_node_identity() {
        char          host[256] = {};
        std::uint64_t h = 0;
        if (::gethostname(host, sizeof host - 1) == 0) {
            h = 1469598103934665603ull;  // FNV-1a offset basis
            for (const char* p = host; *p; ++p) {
                h ^= static_cast<unsigned char>(*p);
                h *= 1099511628211ull;
            }
        }
        m_recorder->record(core::Temporal::none(), core::records::NodeIdentity{{}, {}, h});
    }

    // What each device ordinal in this capture actually refers to. See
    // core/records.hpp GpuDevice for why an ordinal is not an identity and why
    // the key is (node_hash, pci_bus) rather than either half alone.
    //
    // dlopen(RTLD_NOLOAD), which is the one spelling that is both correct and
    // honest here. This must describe GPUs the process can ALREADY see and must
    // never load the driver to ask — loading it would change what the traced
    // program links against, the one thing a tracer may not do. RTLD_NOLOAD
    // returns a handle only if libcuda is already mapped and null otherwise, so
    // it buys visibility without that risk. A capture with no gpu_device rows
    // therefore means "this process had no CUDA", which is itself readable.
    //
    // RTLD_DEFAULT WAS TRIED FIRST AND SILENTLY FOUND NOTHING, which is worth
    // recording because it looks right. libcudart dlopens libcuda.so.1 with
    // RTLD_LOCAL, so the driver's symbols never enter the global namespace and
    // dlsym(RTLD_DEFAULT, "cuInit") is null in a process that is demonstrably
    // using CUDA. Measured on job 45296412: a program calling cudaGetDeviceCount
    // and cudaFree on an H100 produced a capture holding only 524 and 590 — no
    // 528 at all — while nvidia-smi on the same node named the GPU. The failure
    // mode was an empty table, not an error, which is exactly the shape this
    // record exists to stop other tables having.
    //
    // The driver API and not the runtime: libcuda is present whenever CUDA is
    // used at all, including through libcudart, so this sees GPU-enabled runs
    // that never link the runtime themselves.
    //
    // Attributes rather than cuDeviceGetPCIBusId, because that returns
    // "0000:1B:00.0" and parsing a string to rebuild three integers the driver
    // already has is a decoder to get wrong. 33/34/50 are BUS_ID, DEVICE_ID and
    // DOMAIN_ID; they are stable CUDA ABI and are spelled numerically here for
    // the same reason nothing else in this file includes a vendor header.
    //
    // Sets m_gpu_devices_done only on success — see maybe_emit_gpu_devices.
    void emit_gpu_devices() {
        using CuInit = int (*)(unsigned);
        using CuCount = int (*)(int*);
        using CuDevGet = int (*)(int*, int);
        using CuAttr = int (*)(int*, int, int);
        using CuUuid = int (*)(void*, int);
        using CuName = int (*)(char*, int, int);

        void* cuda = ::dlopen("libcuda.so.1", RTLD_NOLOAD | RTLD_LAZY);
        if (!cuda)
            return;  // the driver is not in this process — nothing to describe
        struct Closer {
            void* h;
            ~Closer() { ::dlclose(h); }  // NOLOAD still took a reference
        } closer{cuda};

        auto sym = [cuda](const char* n) { return ::dlsym(cuda, n); };
        auto cu_init = reinterpret_cast<CuInit>(sym("cuInit"));
        auto cu_count = reinterpret_cast<CuCount>(sym("cuDeviceGetCount"));
        auto cu_dev = reinterpret_cast<CuDevGet>(sym("cuDeviceGet"));
        auto cu_attr = reinterpret_cast<CuAttr>(sym("cuDeviceGetAttribute"));
        if (!cu_init || !cu_count || !cu_dev || !cu_attr)
            return;  // a libcuda without these is not one we can read
        // Optional: a driver too old for either still yields a usable row.
        auto cu_uuid = reinterpret_cast<CuUuid>(sym("cuDeviceGetUuid"));
        auto cu_name = reinterpret_cast<CuName>(sym("cuDeviceGetName"));

        // Idempotent by contract, and required before the rest answer. If the
        // application already initialised the driver this is a no-op; if it
        // somehow loaded libcuda without using it, this is what makes the
        // enumeration legal rather than undefined.
        if (cu_init(0) != 0)
            return;
        int n = 0;
        if (cu_count(&n) != 0 || n <= 0)
            return;

        for (int i = 0; i < n; ++i) {
            int dev = 0;
            if (cu_dev(&dev, i) != 0)
                continue;

            int bus = 0, slot = 0, domain = 0;
            // A device whose PCI address will not answer is skipped, not
            // written with zeros: (node_hash, 0) would collide with every other
            // such device on the node, which is the exact merge this record
            // exists to prevent.
            if (cu_attr(&bus, 33, dev) != 0 || cu_attr(&slot, 34, dev) != 0)
                continue;
            if (cu_attr(&domain, 50, dev) != 0)
                domain = 0;  // pre-domain drivers: single domain, so 0 is true

            // 0,0 means "unavailable", which the reader must not mistake for a
            // real all-zero UUID — the degenerate case this field exists to
            // survive. Left zero on failure and cross-checked, never keyed on.
            std::uint64_t hi = 0, lo = 0;
            if (cu_uuid) {
                unsigned char raw[16] = {};
                if (cu_uuid(raw, dev) == 0) {
                    for (int b = 0; b < 8; ++b) {
                        hi = (hi << 8) | raw[b];
                        lo = (lo << 8) | raw[b + 8];
                    }
                }
            }

            core::Handle name = core::kNullHandle;
            if (cu_name) {
                char buf[128] = {};
                if (cu_name(buf, sizeof buf - 1, dev) == 0 && buf[0])
                    name = m_recorder->intern(rec::InternDomain::KernelName, buf);
            }

            m_recorder->record(
                core::Temporal::none(),
                core::records::GpuDevice{
                    {},
                    {},
                    hi,
                    lo,
                    static_cast<std::uint32_t>((static_cast<std::uint32_t>(domain) << 16) |
                                               (static_cast<std::uint32_t>(bus & 0xFF) << 8) |
                                               static_cast<std::uint32_t>(slot & 0xFF)),
                    name,
                    static_cast<core::DeviceId>(i)});
        }
        // Only now: a run that reached here with n > 0 has written every row it
        // is going to, and a failure above leaves the flag clear so the next
        // flush tries again.
        m_gpu_devices_done.store(true, std::memory_order_relaxed);
    }

    // Intern dictionaries, as records plus one byte heap.
    //
    // These were `intern-<domain>.tsv`: bare text with no magic, no version and
    // no schema, holding the only thing that gives a Handle meaning. A reader
    // that could not find a handle returned "" — so a truncated or mismatched
    // dictionary produced empty cells rather than an error, silently voiding
    // every record in the capture. Now the index is records (framed, schema'd,
    // conformance-checked like everything else) and the string bytes are a blob
    // with its own header.
    //
    // Runs at finalize, once, after the application's own records have drained.
    // Nothing here interns, so no new handles appear while we walk the tables.
    void emit_intern_records() {
        auto              blob_path = m_sink->dir() / "intern-blob.bin";
        std::vector<char> blob;

        for (std::size_t d = 0; d < rec::kInternDomainCount; ++d) {
            auto  domain = static_cast<rec::InternDomain>(d);
            auto& table = m_recorder->table(domain);
            if (table.size() == 0)
                continue;

            // Strings: bytes go to the blob, the record keeps (offset, length).
            // The handle comes from the table rather than a counter here — see
            // InternTable::string_handles for why recomputing it was a latent
            // way to write a well-formed dictionary that resolves to the wrong
            // text.
            const auto& texts = table.strings();
            const auto& handles = table.string_handles();
            for (std::size_t i = 0; i < texts.size(); ++i) {
                const auto offset = blob.size();
                blob.insert(blob.end(), texts[i].begin(), texts[i].end());
                m_recorder->record(
                    core::Temporal::none(),
                    core::records::InternString{{},
                                                {},
                                                static_cast<std::uint64_t>(offset),
                                                static_cast<std::uint32_t>(texts[i].size()),
                                                handles[i],
                                                static_cast<std::uint8_t>(d)});
            }
            // Numeric values need no heap: they are fixed width already.
            for (const auto& [key, handle] : table.keys())
                m_recorder->record(
                    core::Temporal::none(),
                    core::records::InternKey{{}, {}, key, handle, static_cast<std::uint8_t>(d)});
        }

        if (blob.empty())
            return;
        std::FILE* f = std::fopen(blob_path.c_str(), "wb");
        if (!f)
            return;
        rec::BlobHeader bh;
        bh.byte_count = blob.size();
        std::fwrite(&bh, sizeof bh, 1, f);
        std::fwrite(blob.data(), 1, blob.size(), f);
        std::fclose(f);
    }

    Config                           m_config{};
    std::unique_ptr<rec::FileSink>   m_sink;
    std::unique_ptr<rec::Recorder<>> m_recorder;
    // How many providers have start()ed and not yet stop()ped. Guards the
    // attach/finalize decisions above; ctors normally run single-threaded under
    // _dl_init, but stop() can arrive from a vendor exit callback on any thread.
    int        m_active{0};
    bool       m_finalized{false};
    std::mutex m_lifecycle;
    // One-shot, and atomic because the arena-flush path that tests it runs on
    // any recording thread. Relaxed throughout: the only cost of two threads
    // both passing the gate is a duplicate enumeration, and the only cost of a
    // stale read is one extra attempt on the next flush. Neither is worth an
    // acquire/release pair on a path this hot.
    std::atomic<bool> m_gpu_devices_done{false};
};

inline Config Config::from_env() {
    Config cfg;
    if (const char* v = std::getenv("PCOMM_OUTPUT_DIR"))
        cfg.output_dir = v;
    if (const char* v = std::getenv("PCOMM_VERBOSE"))
        cfg.verbose = *v == '1';
    // Rank guess (SigmaTrace's RankDetector, minimal port): explicit override,
    // then launcher-provided env, then pid so concurrent unranked processes
    // never clobber each other's streams.
    cfg.rank = -1;
    for (const char* var : {"PCOMM_RANK", "OMPI_COMM_WORLD_RANK", "PMIX_RANK", "PMI_RANK",
                            "SLURM_PROCID", "NVSHMEM_MY_PE"}) {
        if (const char* v = std::getenv(var)) {
            cfg.rank = std::atoi(v);
            break;
        }
    }
    if (cfg.rank < 0)
        cfg.rank = static_cast<int>(getpid());
    return cfg;
}

}  // namespace pcomm
