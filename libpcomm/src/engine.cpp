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
#include "pcomm/hook/dlsym/Dlsym.hpp"
#include "pcomm/core/ids.hpp"
#include "pcomm/core/mem_attr.hpp"
#include "pcomm/core/object_registry.hpp"
#include "pcomm/core/records.hpp"
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

// Registers the ibv_destroy_qp smart_dlsym hook (defined below). Called at
// capture start so it is in place before the application initialises NVSHMEM,
// which resolves ibv_destroy_qp through dlsym rather than the PLT.
static void pcomm_install_ibv_destroy_qp_hook();

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
    pcomm_install_ibv_destroy_qp_hook();

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

// True if some loaded provider declared the id range containing `type_id` — i.e.
// a provider that records this kind of entity is present in this run. Used to
// gate the QP close: on a p2p-only (gpumem) run no provider records queue-pair
// opens, so a close would name an entity nothing created.
bool type_is_declared(std::uint16_t type_id) {
    std::lock_guard lock(declared_mutex());
    for (const auto& d : declared())
        if (type_id >= d.lo && type_id <= d.hi)
            return true;
    return false;
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

/* --- application memory objects -------------------------------------------
 *
 * Interposed HERE, in the engine, because the answer to "what did the program
 * allocate" must not depend on which provider ran. See
 * core/object_registry.hpp for the measurement that forced the move and
 * PLAN-20260810-0505 for the design.
 *
 * The engine is preloaded first, so these definitions win the symbol for the
 * application's calls and the real ones are reached with RTLD_NEXT. The engine
 * already declares record ids 500-599 for itself (clock anchors), and
 * MemObject 520 / EntityClose 522 are in that range — so this emits records the
 * engine already owns rather than borrowing a provider's range.
 *
 * NVSHMEM only, for now. nvshmem_malloc never reaches any CUDA allocation API:
 * it sub-allocates from the pre-mapped symmetric heap through NVSHMEM's own
 * mspace (src/host/mem/mem_heap.cpp -> custom_malloc.cpp, a dlmalloc
 * derivative), so no interposer below this layer can ever see a buffer. Plain
 * cuMemAlloc is deliberately NOT taken: NVBit already sees it and gpumem needs
 * it in its own registry regardless, so moving it would buy nothing today.
 *
 * nvshmemi_malloc — NVSHMEM's internal allocator — is deliberately not hooked.
 * Those are NVSHMEM's bookkeeping, not the application's data, and recording
 * them would bury the objects a user actually asked for.
 */
namespace {

template<typename Fn> Fn pcomm_next_sym(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

void pcomm_note_object(void* p, std::size_t size) {
    if (!p || !size)
        return;
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return; /* nothing has started a capture; the app is running untraced */
    const auto base = reinterpret_cast<std::uint64_t>(p);
    auto&      rec = tracer.recorder();
    /* Interned in the Pointer domain — the same identity scheme ibverbs,
     * mlx5dv and gpumem use, so the same buffer mints the same id everywhere
     * and the rows merge instead of doubling. */
    const auto object = rec.intern(pcomm::rec::InternDomain::Pointer, base);
    const auto device = pcomm::core::device_of(p);
    rec.record(rec.now_instant(),
               pcomm::core::records::MemObject{
                   {}, {}, object, pcomm::core::kNullHandle, {base, size}, device});
    pcomm::core::ObjectRegistry::instance().add(base, size, object, device);
}

void pcomm_close_object(void* p) {
    if (!p)
        return;
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return;
    const auto base = reinterpret_cast<std::uint64_t>(p);
    const auto object = pcomm::core::ObjectRegistry::instance().object_at(base);
    if (object == pcomm::core::kNullHandle)
        return; /* allocated before we attached: closing it would manufacture
                 * the unmatched row entity_close_accounted exists to detect */
    auto& rec = tracer.recorder();
    rec.record(rec.now_instant(),
               pcomm::core::records::EntityClose{
                   {}, {}, object, pcomm::core::records::MemObject::pcomm_type_id});
}

/* Queue pairs whose OPEN a provider recorded, keyed by the ibv_qp* the creation
 * door returned — the per-QP counterpart of ObjectRegistry, and what lets the
 * engine's ibv_destroy_qp close exactly the queue pairs some provider created.
 * Populated through pcomm_engine_note_qp.
 *
 * Unlike ObjectRegistry this one ERASES on close, and the difference is not an
 * inconsistency. An object's extent has to stay resolvable after the free — a
 * GPU access still draining is attributed against the [open, close) window it
 * fell in — whereas nothing ever resolves an address to a queue pair. What the
 * entry is for is answering "was this handle's open recorded", and a destroyed
 * handle's address is handed straight back to the next ibv_create_qp: keeping it
 * would let the NEXT queue pair at that address, created through a door we do
 * not hook, be closed as if its open had been recorded — the missing-door signal
 * swallowed by a stale entry. Erasing also makes a second destroy of the same
 * pointer fall to the unregistered path, where the assertion can see it. */
class QpRegistry {
public:
    static QpRegistry& instance() {
        static auto* self = new QpRegistry();  // leaked, like every engine global
        return *self;
    }

    void add(std::uint64_t qp, std::uint16_t opener) {
        std::lock_guard lock(m_mutex);
        m_qps[qp] = opener;
    }

    // The opener type recorded for `qp`, and forgets it. 0 if no open was
    // recorded for this handle.
    std::uint16_t take(std::uint64_t qp) {
        std::lock_guard lock(m_mutex);
        auto            it = m_qps.find(qp);
        if (it == m_qps.end())
            return 0;
        const auto opener = it->second;
        m_qps.erase(it);
        return opener;
    }

private:
    std::mutex                                       m_mutex;
    std::unordered_map<std::uint64_t, std::uint16_t> m_qps;
};

/* Queue-pair teardown. Subject is the interned QP pointer, which is unique
 * across providers, so it matches whichever open recorded it. Named by type id
 * here rather than referenced from IbQueuePair because the engine is vendor-free
 * and cannot include a provider's records.
 *
 * Two paths, because there are two things a destroy can mean:
 *
 *  - The open WAS recorded (the registry knows this handle). Close it, and report
 *    the opener the creating provider registered — 102 or 210, the door that
 *    actually made this queue pair, rather than the 102 the closing hook used to
 *    assume for all of them.
 *
 *  - No open was recorded. That is either the signal we want — a creation door we
 *    do not hook, the mirror of case 08's unaccounted queue pair — or a queue
 *    pair belonging to a provider the user did not select. The engine cannot tell
 *    those apart from a void*: an ibv_create_qp queue pair on a `-p mlx5dv` run
 *    and one mlx5dv_create_qp missed on that same run are the same pointer with
 *    the same absent open. So the coarse per-run rule stays, for this path only:
 *    emit the close if SOME QP-recording provider is loaded (102 ibverbs or 210
 *    mlx5dv declared), so entity_close_accounted still fires on the missing door;
 *    stay silent on a p2p-only gpumem run, where no provider records queue pairs
 *    at all and every close would be noise. */
void pcomm_close_qp(void* p) {
    if (!p)
        return;
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return;
    constexpr pcomm::core::RecordTypeId kIbQueuePairTypeId = 102;
    const auto                          addr = reinterpret_cast<std::uint64_t>(p);
    const std::uint16_t                 registered = QpRegistry::instance().take(addr);
    if (!registered && !type_is_declared(102) && !type_is_declared(210))
        return;
    auto& rec = tracer.recorder();
    rec.record(rec.now_instant(), pcomm::core::records::EntityClose{
                                      {},
                                      {},
                                      rec.intern(pcomm::rec::InternDomain::Pointer, addr),
                                      registered ? registered : kIbQueuePairTypeId});
}

}  // namespace

extern "C" void* nvshmem_malloc(size_t size) {
    static auto real = pcomm_next_sym<void* (*)(size_t)>("nvshmem_malloc");
    void*       p = real ? real(size) : nullptr;
    pcomm_note_object(p, size);
    return p;
}

extern "C" void* nvshmem_calloc(size_t count, size_t size) {
    static auto real = pcomm_next_sym<void* (*)(size_t, size_t)>("nvshmem_calloc");
    void*       p = real ? real(count, size) : nullptr;
    pcomm_note_object(p, count * size);
    return p;
}

extern "C" void* nvshmem_align(size_t alignment, size_t size) {
    static auto real = pcomm_next_sym<void* (*)(size_t, size_t)>("nvshmem_align");
    void*       p = real ? real(alignment, size) : nullptr;
    pcomm_note_object(p, size);
    return p;
}

/* Closed BEFORE the real free, while the pointer still names a live entity: the
 * address can be handed to a new allocation the moment the call returns, and
 * the other order would let that new object be registered and then closed. */
extern "C" void nvshmem_free(void* p) {
    static auto real = pcomm_next_sym<void (*)(void*)>("nvshmem_free");
    pcomm_close_object(p);
    if (real)
        real(p);
}

/* Queue-pair destruction, owned by the ENGINE and not by a provider. A QP
 * created through ibv_create_qp (ibverbs) and one created through
 * mlx5dv_create_qp (mlx5dv) are BOTH torn down through ibv_destroy_qp, the two
 * providers coexist by design, and interposition wants exactly one owner of a
 * symbol — so no provider can own this door and account both. The engine can: it
 * is preloaded first in every selection. Before this, a `-p mlx5dv`-only IBGDA
 * run (cases 09/10/12) left every Mlx5QueuePair opened and never closed, because
 * the old owner, ibverbs, was not in the process at all.
 *
 * There are TWO ways in, and both must land in one place so a QP is closed
 * exactly once. perftest and pingpong link libibverbs and call through the PLT,
 * caught by the exported symbol below. NVSHMEM reaches ibv_destroy_qp entirely
 * through dlsym+dlvsym (measured on du04, case 09: `[pcomm-entry] dlsym/dlvsym
 * ibv_destroy_qp`) and calls the real symbol through that pointer, which no ELF
 * interposer ever sees — caught by the smart_dlsym registration in
 * pcomm_install_ibv_destroy_qp_hook. */
namespace {

using PfnDestroyQp = int (*)(void*);

// The real ibv_destroy_qp to forward to, set from the smart_dlsym chain's
// `next` — the EXACT symbol the caller resolved, version included. This matters:
// NVSHMEM resolves ibv_destroy_qp@IBVERBS_1.1 through dlvsym, and a bare
// RTLD_NEXT lookup can land on a different (compat) implementation that then
// faults on a QP the versioned one owns — the SIGSEGV observed on du04 case 09.
PfnDestroyQp& real_destroy_qp() {
    static PfnDestroyQp p = nullptr;
    return p;
}

// Destroy, then record the close on success — a failed destroy freed nothing.
// The void* is ABI-identical to ibv_qp* and never dereferenced, so the engine
// stays vendor-header-free. Both doors below land here, so a QP is closed
// exactly once however it was reached. Interning the pointer value the instant
// the real call returns is safe: no other allocation runs before this thread
// returns to the caller, so the handle still names this QP.
int ibv_destroy_qp_hook(void* qp) {
    PfnDestroyQp real = real_destroy_qp();
    if (!real) {
        // No dlsym registration captured `next` yet — a pure-PLT caller like
        // perftest/pingpong. Resolve from libibverbs' OWN handle, as
        // PreloadHookManager does: RTLD_NEXT lands on a compat implementation
        // (the du04/MN5 SIGSEGV) and RTLD_DEFAULT finds the engine's own exported
        // ibv_destroy_qp and recurses. dlopen(NOLOAD) returns the already-loaded
        // libibverbs, and real_dlsym on that handle gives its default, correctly
        // versioned symbol without going through the engine's interposition.
        if (pcomm::smart_dlsym::real_dlopen && pcomm::smart_dlsym::real_dlsym) {
            void* h = pcomm::smart_dlsym::real_dlopen("libibverbs.so.1",
                                                      pcomm::smart_dlsym::DL_RTLD_NOW |
                                                          pcomm::smart_dlsym::DL_RTLD_NOLOAD);
            if (h)
                real = reinterpret_cast<PfnDestroyQp>(
                    pcomm::smart_dlsym::real_dlsym(h, "ibv_destroy_qp"));
        }
        real_destroy_qp() = real;
    }
    const int ret = real ? real(qp) : -1;
    if (ret == 0)
        pcomm_close_qp(qp);
    return ret;
}

}  // namespace

// The PLT door: exported global so a direct caller binds here.
extern "C" int ibv_destroy_qp(void* qp) {
    return ibv_destroy_qp_hook(qp);
}

// The dlsym door: hand callers that resolve ibv_destroy_qp dynamically the same
// hook, and capture the exact real symbol they resolved (its version included).
// Modelled on core/dmabuf_registry.hpp's cuGetProcAddress registration.
static void pcomm_install_ibv_destroy_qp_hook() {
    if (!pcomm::smart_dlsym::RegisteredSymbols)
        return;  // dlsym interposer not initialised; nothing resolves through us
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    pcomm::smart_dlsym::register_symbol(
        "ibv_destroy_qp", [](void* next, void*, const char*) -> void* {
            if (next)
                real_destroy_qp() = reinterpret_cast<PfnDestroyQp>(next);
            return reinterpret_cast<void*>(&ibv_destroy_qp_hook);
        });
}

/* Innermost object containing `addr`, for providers that must resolve an access
 * to an object at runtime. Asking the CORE is not the coupling
 * PLAN-20260806-1329 forbids — that rule is about one provider waiting on
 * another; this is the same shape as core/mem_attr.hpp's device_of. */
extern "C" int pcomm_engine_object_at(uint64_t addr, uint64_t* base, uint64_t* size,
                                      uint32_t* object, uint16_t* device) {
    const auto found = pcomm::core::ObjectRegistry::instance().find(addr);
    if (!found)
        return 0;
    if (base)
        *base = found->base;
    if (size)
        *size = found->size;
    if (object)
        *object = found->object;
    if (device)
        *device = found->device;
    return 1;
}

/* A provider recorded a queue-pair open; remember the handle so the engine's
 * ibv_destroy_qp closes THIS queue pair knowingly, and reports the door that
 * created it. See QpRegistry and pcomm_close_qp for what the two paths mean. */
extern "C" void pcomm_engine_note_qp(void* qp, uint16_t opener_type_id) {
    if (!qp || !opener_type_id)
        return;
    QpRegistry::instance().add(reinterpret_cast<std::uint64_t>(qp), opener_type_id);
}
