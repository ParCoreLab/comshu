// The engine's one and only definition site.
//
// Everything here used to be a function-local static inside a header, which
// meant every provider .so carried its own copy with weak (vague) linkage. The
// dynamic linker then bound all references to whichever copy loaded first, so
// two providers shared a Tracer *by accident* — and destroyed each other's
// Recorder on the way up (see commit 7a5b55a, things-to-circle-back-to.md).
//
// Defining them in exactly one shared object makes that sharing deliberate:
// one Tracer, one buffer registry, one id tracker, one set of intern tables,
// no matter how many providers are loaded. The headers now only *declare*
// these, so a provider physically cannot emit a second copy.
//
// INVARIANT: nothing may be added here that pulls in a vendor header — this is
// still L0-L2 core (see ARCHITECTURE.md §0).

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pcomm/engine_abi.h"
#include "pcomm/core/ids.hpp"
#include "pcomm/core/record.hpp"
#include "pcomm/rec/recorder.hpp"
#include "pcomm/rec/thread_buffer_registry.hpp"
#include "pcomm/tracer.hpp"

namespace pcomm {

// All three are deliberately leaked (never destroyed): backends flush from
// exit-time vendor callbacks (NVBit's atexit, CUPTI's) that can run *after*
// C++ static destructors, so a destroyed singleton would be a use-after-free
// on the way out. The leak is process-exit-only.

Tracer& Tracer::instance() {
    static Tracer* t = new Tracer();
    return *t;
}

namespace rec {

ThreadBufferRegistry& ThreadBufferRegistry::instance() {
    static ThreadBufferRegistry* r = new ThreadBufferRegistry();
    return *r;
}

}  // namespace rec

namespace core {

RecordIdTracker& RecordIdTracker::instance() {
    static RecordIdTracker* t = new RecordIdTracker();
    return *t;
}

}  // namespace core

// ── C ABI implementation (engine_abi.h) ─────────────────────────────────────
namespace {

// The engine-side arena. `pub` is the only part a provider ever sees, and it is
// plain POD — which is the point: gpumem is built by nvcc/gcc-11.4 and this
// object by gcc-13.2, so nothing with a C++ ABI may cross between them.
struct Arena final : rec::IDrainable {
    pcomm_arena            pub{};
    std::vector<std::byte> storage;
    core::RecordTypeId     type_id{};
    core::RecordKind       kind{};

    Arena(core::RecordTypeId id, core::RecordKind k, std::uint32_t stride, std::uint32_t capacity)
        : storage(static_cast<std::size_t>(stride) * capacity), type_id(id), kind(k) {
        pub.slots = storage.data();
        pub.count = 0;
        pub.capacity = capacity;
        pub.stride = stride;
        pub._owner = this;
    }

    void drain() override {
        if (pub.count == 0)
            return;
        auto& tracer = Tracer::instance();
        if (!tracer.has_recorder()) {
            pub.count = 0;
            return;
        }
        tracer.recorder().sink().submit(rec::BufferView{
            .type_id = type_id,
            .kind = kind,
            .data = static_cast<const std::byte*>(pub.slots),
            .bytes = std::size_t{pub.count} * pub.stride,
            .count = pub.count,
        });
        pub.count = 0;
    }
};

// Per-thread arena table, keyed by record type. Leaked on purpose (raw pointer,
// no TLS destructor): glibc destroys main-thread TLS *before* atexit handlers
// and GPU backends flush from those, so the arenas must outlive their thread.
using ArenaMap = std::unordered_map<core::RecordTypeId, std::unique_ptr<Arena>>;
ArenaMap& arenas_for_this_thread() {
    thread_local ArenaMap* m = nullptr;
    if (!m)
        m = new ArenaMap();
    return *m;
}

}  // namespace
}  // namespace pcomm

static void warn_if_undeclared(std::uint16_t type_id);

extern "C" {

std::uint32_t pcomm_engine_abi_version(void) {
    return PCOMM_ENGINE_ABI_VERSION;
}

int pcomm_engine_acquire(const char* output_dir, int rank, int verbose) {
    pcomm::Config cfg;
    if (output_dir && *output_dir)
        cfg.output_dir = output_dir;
    cfg.rank = rank;
    cfg.verbose = verbose != 0;
    // The engine emits its own records (clock anchors, 500-599) and is
    // therefore a provider of record ids too. Declaring here rather than
    // special-casing the check keeps one rule: every id in a capture belongs to
    // exactly one declared range.
    pcomm_engine_declare_provider("pcomm-engine", pcomm::core::id_range::internal_lo,
                                  pcomm::core::id_range::internal_hi, PCOMM_ENGINE_ABI_VERSION);

    pcomm::Tracer::instance().start(cfg);

    // Loud, once: a linked-but-not-preloaded engine still "works" — it just
    // captures less, with no error anywhere. That cost 12 of 16 memory regions
    // and 36 of 432 queue pairs, and only a sufficiently detailed golden
    // noticed. Never let that be silent again.
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (!pcomm_engine_interposer_live())
            std::fprintf(stderr, "[pcomm] WARNING: libpcomm_engine.so is not the resolver for "
                                 "`dlsym` — it was linked but not PRELOADED. Preload-based "
                                 "capture will be silently incomplete. Put the engine first in "
                                 "LD_PRELOAD (the pcomm launcher does this; PCOMM_ENGINE_PATH "
                                 "overrides the location).\n");
        else if (cfg.verbose)
            std::fprintf(stderr, "[pcomm] interposer live: dlsym resolves to the engine\n");
    }
    return 0;
}

void pcomm_engine_release(void) {
    pcomm::Tracer::instance().stop();
}

pcomm_arena* pcomm_engine_arena(std::uint16_t type_id, std::uint8_t kind, std::uint32_t stride,
                                std::uint32_t capacity) {
    auto& map = pcomm::arenas_for_this_thread();
    auto  it = map.find(type_id);
    if (it == map.end()) {
        warn_if_undeclared(type_id);
        auto a = std::make_unique<pcomm::Arena>(type_id, static_cast<pcomm::core::RecordKind>(kind),
                                                stride, capacity);
        // Registered so Tracer::finalize() can flush arenas belonging to threads
        // that never call drain_thread() (NCCL's proxy threads, NVBit's).
        pcomm::rec::ThreadBufferRegistry::instance().register_buffer(a.get());
        it = map.emplace(type_id, std::move(a)).first;
    }
    return &it->second->pub;
}

void pcomm_engine_arena_flush(pcomm_arena* arena) {
    if (arena && arena->_owner)
        static_cast<pcomm::Arena*>(arena->_owner)->drain();
}

void pcomm_engine_drain_thread(void) {
    for (auto& [id, a] : pcomm::arenas_for_this_thread())
        a->drain();
}

void pcomm_engine_reset_arenas_for_fork(void) {
    // Only the forking thread exists in the child, so its map is the only one
    // that can still be reached — and it is the one holding the parent's
    // buffered records.
    for (auto& [id, a] : pcomm::arenas_for_this_thread()) {
        a->pub.count = 0;  // drop the parent's records; the child starts empty
        pcomm::rec::ThreadBufferRegistry::instance().register_buffer(a.get());
    }
}

std::uint32_t pcomm_engine_intern_str(std::uint8_t domain, const char* s, std::size_t n) {
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return 0;
    auto            d = static_cast<pcomm::rec::InternDomain>(domain);
    auto&           rec = tracer.recorder();
    std::lock_guard lock(rec.mutex(d));
    return rec.table(d).intern(std::string_view(s, n));
}

std::uint32_t pcomm_engine_intern_key(std::uint8_t domain, std::uint64_t key) {
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return 0;
    auto            d = static_cast<pcomm::rec::InternDomain>(domain);
    auto&           rec = tracer.recorder();
    std::lock_guard lock(rec.mutex(d));
    return rec.table(d).intern(key);
}

}  // extern "C"

// Where this .so lives on disk, so the launcher can put it in LD_PRELOAD.
//
// It MUST be preloaded, not merely linked: the dynamic linker searches
// preloaded objects before the executable's own dependencies, and `dlsym` lives
// here. As a plain DT_NEEDED of a preloaded provider the engine loses the
// `dlsym` lookup to libc, our interposer never runs, and every symbol a target
// resolves at runtime (NVSHMEM resolves its verbs entry points that way) is
// captured only if it also happens to go through a PLT we interpose. That cost
// 12 of 16 memory regions and 36 of 432 queue pairs before it was caught.
//
// Self-reporting via dladdr rather than making the launcher guess a path keeps
// this correct across build layouts (build/ vs build-gpumem/, installed, or a
// provider .so copied elsewhere).
// --- step 3/4: enforcement at the boundary ---------------------------------
//
// Every bug in this refactor failed by producing a plausible green rather than
// an error: stale suite captures inheriting an older PASS, the case 07 label
// mismatch that left an xfail unverified for days, and the dlsym loss that
// silently dropped 12 of 16 memory regions. Only a golden with enough entities
// in it caught the last one. These two checks turn the remaining instances of
// that class into loud failures.

namespace {

struct DeclaredProvider {
    std::string   name;
    std::uint16_t lo{0}, hi{0};
};

std::vector<DeclaredProvider>& declared() {
    static auto* v = new std::vector<DeclaredProvider>();
    return *v;
}

std::mutex& declared_mutex() {
    static auto* m = new std::mutex();
    return *m;
}

}  // namespace

extern "C" int pcomm_engine_declare_provider(const char* name, std::uint16_t id_lo,
                                             std::uint16_t id_hi, std::uint32_t abi_version) {
    const char* who = name ? name : "?";
    if (abi_version != PCOMM_ENGINE_ABI_VERSION) {
        std::fprintf(stderr,
                     "[pcomm] provider '%s' was built against engine ABI %u but this "
                     "engine is ABI %u — refusing. Rebuild the provider.\n",
                     who, abi_version, PCOMM_ENGINE_ABI_VERSION);
        return 1;
    }
    if (id_lo > id_hi) {
        std::fprintf(stderr,
                     "[pcomm] provider '%s' declares an inverted id "
                     "range [%u,%u] — refusing.\n",
                     who, id_lo, id_hi);
        return 1;
    }
    std::lock_guard lock(declared_mutex());
    for (const auto& d : declared()) {
        if (d.name == who && d.lo == id_lo && d.hi == id_hi)
            return 0;  // idempotent
        // pcomm-read merges provider streams by record-type id and assumes the
        // ranges are disjoint. Nothing enforced that until now.
        if (id_lo <= d.hi && d.lo <= id_hi) {
            std::fprintf(stderr,
                         "[pcomm] provider '%s' claims record ids [%u,%u], which overlaps "
                         "'%s' [%u,%u]. Streams would collide and pcomm-read would merge "
                         "unrelated records — refusing.\n",
                         who, id_lo, id_hi, d.name.c_str(), d.lo, d.hi);
            return 1;
        }
    }
    declared().push_back({who, id_lo, id_hi});
    return 0;
}

// Warn once per offending type id: a record outside every declared range means
// a provider is emitting into id space it never claimed, which is exactly what
// the disjointness check above exists to prevent.
static void warn_if_undeclared(std::uint16_t type_id) {
    std::lock_guard lock(declared_mutex());
    if (declared().empty())
        return;  // nobody declared; nothing to check against
    for (const auto& d : declared())
        if (type_id >= d.lo && type_id <= d.hi)
            return;
    static auto* warned = new std::unordered_map<std::uint16_t, bool>();
    if (warned->emplace(type_id, true).second)
        std::fprintf(stderr,
                     "[pcomm] record type %u is outside every declared provider id "
                     "range — the capture may be unreadable or merged wrongly.\n",
                     type_id);
}

extern "C" int pcomm_engine_interposer_live(void) {
    // Resolve "dlsym" the way an external caller would, then ask which object
    // actually defines what came back. If it is not us, we were linked rather
    // than preloaded and preload-based capture is silently incomplete.
    void* sym = ::dlsym(RTLD_DEFAULT, "dlsym");
    if (!sym)
        return 0;
    Dl_info theirs{}, ours{};
    if (!dladdr(sym, &theirs) || !theirs.dli_fname)
        return 0;
    if (!dladdr(reinterpret_cast<const void*>(&pcomm_engine_interposer_live), &ours) ||
        !ours.dli_fname)
        return 0;
    return std::string(theirs.dli_fname) == std::string(ours.dli_fname) ? 1 : 0;
}

extern "C" const char* pcomm_engine_so_path() {
    static const std::string path = [] {
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&pcomm_engine_so_path), &info) && info.dli_fname)
            return std::string(info.dli_fname);
        return std::string();
    }();
    return path.empty() ? nullptr : path.c_str();
}
