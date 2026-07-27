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
#include "pcomm/engine_abi.h"
#include "pcomm/rec/sink.hpp"
#include "pcomm/rec/thread_buffer_registry.hpp"
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

    // Provider-side proxy. Every recording operation now goes through the C ABI
    // in engine_abi.h, so a provider needs no sink and — importantly — must not
    // hold a reference into the engine's Recorder: that would put a C++ object
    // layout back on the cross-compiler boundary this ABI exists to remove.
    Recorder() = default;

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Arenas are owned by the engine (see engine_abi.h) and outlive any single
    // Recorder, so there is nothing to reclaim here. Cross-thread flush is
    // ThreadBufferRegistry's job, driven from Tracer::finalize().
    ~Recorder() = default;

    // --- recording -----------------------------------------------------------

    // Auto-timestamped instant Event (or timeless record when R is an
    // Artifact/Edge — pass Temporal explicitly for those if needed).
    template <core::Record R>
    void record(const R& r) { append(now_instant(), r); }

    template <core::Record R>
    void record(const core::Temporal& t, const R& r) { append(t, r); }

    // Per-thread, per-(recorder, record-type) buffer. Hoist this lookup out of
    // hot loops (it is a map lookup in the skeleton; the production version
    // caches the pointer in the event source).
    // Append into this thread's arena for R. The arena pointer is cached in a
    // thread_local, so the steady-state cost is a bounds check and two stores —
    // the old path did a std::map lookup keyed {recorder, type_id} per record.
    //
    // Crucially this is also the ABI boundary: only POD crosses into the engine.
    // gpumem is compiled by nvcc/gcc-11.4 and the engine by gcc-13.2; the old
    // code had that gcc-11.4 inline calling operator[] on a gcc-13.2 std::map.
    template <core::Record R>
    void append(const core::Temporal& t, const R& r) {
        static thread_local pcomm_arena* arena = pcomm_engine_arena(
            core::RecordTrait<R>::type_id,
            static_cast<std::uint8_t>(core::RecordTrait<R>::kind),
            static_cast<std::uint32_t>(sizeof(Stamped<R>)),
            static_cast<std::uint32_t>(Config::buffer_capacity));
        if (arena->count == arena->capacity) pcomm_engine_arena_flush(arena);
        auto* slot = reinterpret_cast<Stamped<R>*>(
            static_cast<std::byte*>(arena->slots) + std::size_t{arena->count} * arena->stride);
        slot->temporal = t;
        slot->payload  = r;
        ++arena->count;
    }

    // --- interning -----------------------------------------------------------
    // NOTE(skeleton): process-wide tables with a per-domain lock, so handles
    // are consistent across threads (instrumentation thread interns the same
    // pc/file the recv thread references). The lock-free per-thread design
    // from UNIFICATION §3.3 (merge in the reader) replaces this later.

    core::Handle intern(InternDomain d, std::string_view v) {
        return pcomm_engine_intern_str(static_cast<std::uint8_t>(d), v.data(), v.size());
    }
    core::Handle intern(InternDomain d, std::uint64_t key) {
        return pcomm_engine_intern_key(static_cast<std::uint8_t>(d), key);
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

public:
    // Drains every arena this thread owns. Recording threads must call this
    // before exiting: arenas deliberately have no TLS destructor, because glibc
    // tears down main-thread TLS *before* atexit handlers and GPU backends
    // flush from those.
    void drain_thread() { pcomm_engine_drain_thread(); }

private:

    ISink*         m_sink;
    core::DomainId m_cpu_domain;
};


// NB deliberately NOT `extern template` any more. It was needed while the
// Recorder owned state (buffer_store, intern tables) that had to exist once per
// process; that state now lives behind the C ABI in the engine, so the class is
// a stateless inline wrapper. Suppressing instantiation here would make a
// provider *import* record/intern/drain_thread as mangled C++ symbols from the
// engine — putting gcc-11.4 -> gcc-13.2 calls back on the boundary this ABI
// exists to remove. Let them inline; the only members with function-local
// statics (table/mutex) are engine-internal and simply aren't instantiated in a
// provider that never calls them.

}  // namespace pcomm::rec
