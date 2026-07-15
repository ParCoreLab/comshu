#pragma once
// L1 — Recorder: the facade providers hold. One per process (owned by the
// Tracer), fronting per-thread per-record-type buffers plus intern tables.
//
// Config carries the TimestampSource (Strategy, resolved at compile time —
// the hot path makes zero virtual calls; the only vtable is ISink, invoked
// on buffer drain).

#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "pcomm/core/record.hpp"
#include "pcomm/core/time.hpp"
#include "pcomm/helpers/clock.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/rec/sink.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

namespace pcomm::rec {

struct DefaultConfig {
    using TimestampSource = helpers::TscClock;
    static constexpr std::size_t buffer_capacity = 4096;
};

template <class Config = DefaultConfig>
class Recorder {
public:
    // The recorder does not own the sink: the Tracer wires one in and controls
    // its lifetime (it must outlive all recording threads).
    explicit Recorder(ISink& sink, core::DomainId cpu_domain = 0)
        : m_sink(&sink), m_cpu_domain(cpu_domain) {}

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Drains and releases this recorder's buffers on the destroying thread.
    // NOTE(skeleton): buffers created by *other* threads are drained at those
    // threads' exit; the production ThreadBufferRegistry (from SigmaTrace)
    // will own cross-thread flush instead.
    ~Recorder() {
        auto& s = buffer_store();
        for (auto it = s.begin(); it != s.end();) {
            if (it->first.first == this) it = s.erase(it);
            else ++it;
        }
    }

    // --- recording -----------------------------------------------------------

    // Auto-timestamped instant Event (or timeless record when R is an
    // Artifact/Edge — pass Temporal explicitly for those if needed).
    template <core::Record R>
    void record(const R& r) {
        buffer<R>().record(now_instant(), r);
    }

    template <core::Record R>
    void record(const core::Temporal& t, const R& r) {
        buffer<R>().record(t, r);
    }

    // Per-thread, per-(recorder, record-type) buffer. Hoist this lookup out of
    // hot loops (it is a map lookup in the skeleton; the production version
    // caches the pointer in the event source).
    template <core::Record R>
    ThreadLocalBuffer<R, Config::buffer_capacity>& buffer() {
        using Buf = ThreadLocalBuffer<R, Config::buffer_capacity>;
        auto& slot = buffer_store()[{this, core::RecordTrait<R>::type_id}];
        if (!slot) {
            bool ok = core::RecordIdTracker::instance().claim(
                core::RecordTrait<R>::type_id, core::RecordTrait<R>::name);
            assert(ok && "duplicate RecordTypeId");
            (void)ok;
            slot = std::make_unique<Buf>(*m_sink);
        }
        return *static_cast<Buf*>(slot.get());
    }

    // --- interning -----------------------------------------------------------
    // NOTE(skeleton): process-wide tables with a per-domain lock, so handles
    // are consistent across threads (instrumentation thread interns the same
    // pc/file the recv thread references). The lock-free per-thread design
    // from UNIFICATION §3.3 (merge in the reader) replaces this later.

    core::Handle intern(InternDomain d, std::string_view v) {
        std::lock_guard lock(mutex(d));
        return table(d).intern(v);
    }
    core::Handle intern(InternDomain d, std::uint64_t key) {
        std::lock_guard lock(mutex(d));
        return table(d).intern(key);
    }

    // Direct table access for the flush-time sidecar dump only (single
    // threaded by then); take mutex(d) yourself if racing recorders.
    // Leaked (never destroyed): GPU backends flush from exit-time vendor
    // callbacks that run after static destructors.
    InternTable& table(InternDomain d) {
        static auto* tables = new std::array<InternTable, kInternDomainCount>;
        return (*tables)[static_cast<std::size_t>(d)];
    }

    std::mutex& mutex(InternDomain d) {
        static auto* mutexes = new std::array<std::mutex, kInternDomainCount>;
        return (*mutexes)[static_cast<std::size_t>(d)];
    }

    // --- time ----------------------------------------------------------------

    core::TimePoint now() const {
        return {m_cpu_domain, Config::TimestampSource::now()};
    }
    core::Temporal now_instant() const { return core::Temporal::instant(now()); }

    ISink& sink() { return *m_sink; }

private:
    using BufferKey = std::pair<const void*, core::RecordTypeId>;
    using BufferStore = std::map<BufferKey, std::unique_ptr<IDrainable>>;

    // Leaked per-thread store (no TLS destructor): glibc destroys main-thread
    // TLS *before* atexit handlers, and GPU backends flush from those. The
    // cost is that buffers do NOT auto-drain at thread exit — every recording
    // thread must call drain_thread() (or drain its buffers) before exiting.
    static BufferStore& buffer_store() {
        thread_local BufferStore* store = nullptr;  // constant-init: no TLS dtor
        if (!store) store = new BufferStore();
        return *store;
    }

public:
    // Drains every buffer this recorder owns on the calling thread.
    void drain_thread() {
        for (auto& [key, buf] : buffer_store()) {
            if (key.first == this) buf->drain();
        }
    }

private:

    ISink*         m_sink;
    core::DomainId m_cpu_domain;
};

}  // namespace pcomm::rec
