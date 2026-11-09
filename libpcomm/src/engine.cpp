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
#include <sys/stat.h>
#include <unistd.h>  // getpid, and glibc's declaration of `environ`

#include <atomic>
#include <csignal>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Regenerated on every build by cmake/write_build_id.cmake, into this target's
// binary dir. Private to pcomm_engine: no provider and no header sees it, so
// there is exactly one object in the process that can answer "which tree".
#include "pcomm_build_id.h"

// POSIX spells it here rather than in a header on every platform: glibc only
// declares it in <unistd.h> under _GNU_SOURCE. Declared once, at file scope, so
// Invocation's env capture does not depend on which feature macros happen to be
// set for this translation unit.
extern "C" char** environ;

// pcomm::maybe_emit_invocation() is declared in pcomm/tracer.hpp (included
// below) and defined further down this file, beside the provider registry and
// the build-id header it reads. Both of its call sites — the arena flush here
// and Tracer::finalize() there — reach it through that one declaration.

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

// --- kernel lifetimes -------------------------------------------------------
//
// The engine owns GpuKernel (530) but measures nothing: the instruments live
// behind vendor headers this file may not include. See engine_abi.h for why
// there are two of them and why neither can be the only one.

void pcomm_engine_record_kernel(std::uint64_t t0, std::uint64_t t1, std::uint16_t domain,
                                const char* name, std::uint64_t correlation, std::uint64_t stream,
                                std::uint16_t device, std::uint8_t source,
                                const std::uint32_t* grid, const std::uint16_t* block) {
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return;  // nothing has started a capture
    auto& rec = tracer.recorder();

    // KernelName, the domain that already exists for exactly this — so two
    // backends observing the same kernel mint the same handle and the rows
    // merge rather than double, the identity rule MemObject's base follows.
    const auto sym = (name && *name) ? rec.intern(pcomm::rec::InternDomain::KernelName, name)
                                     : pcomm::core::kNullHandle;

    // Interval, always: [a, b] is the execution window. A source that only has
    // one instant must still say so through `source`, not by collapsing b onto
    // a, because a zero-width kernel and a submission timestamp are different
    // claims and the reader has to be able to tell them apart.
    rec.record(pcomm::core::Temporal::interval({domain, t0}, {domain, t1}),
               pcomm::core::records::GpuKernel{
                   {},
                   {},
                   correlation,
                   stream,
                   sym,
                   // static_cast around the WHOLE ternary: the conditional
                   // operator applies the usual arithmetic conversions, so
                   // `cond ? uint16 : uint16` is still an int, and a braced
                   // initializer refuses to narrow it back.
                   static_cast<std::uint32_t>(grid ? grid[0] : 0),
                   static_cast<std::uint32_t>(grid ? grid[1] : 0),
                   static_cast<std::uint32_t>(grid ? grid[2] : 0),
                   device,
                   static_cast<std::uint16_t>(block ? block[0] : 0),
                   static_cast<std::uint16_t>(block ? block[1] : 0),
                   static_cast<std::uint16_t>(block ? block[2] : 0),
                   source,
               });
}

// --- host-initiated copies --------------------------------------------------
//
// The engine owns CudaMemcpy (534) and, exactly as with GpuKernel, measures
// nothing: the instruments live behind vendor headers this file may not
// include. See engine_abi.h.
//
// No interning at all, deliberately, which is why this is shorter than
// record_kernel: every field is a number. A copy has no name — CUPTI reports no
// symbol and no address for one (the activity records carry neither) — so there
// is nothing here that could need the dictionary, and the record can therefore
// be written from any thread at any point in the run without the teardown
// ordering Invocation has to worry about.
void pcomm_engine_record_memcpy(std::uint64_t t0, std::uint64_t t1, std::uint16_t domain,
                                std::uint64_t bytes, std::uint64_t correlation,
                                std::uint64_t stream, std::uint16_t src_device,
                                std::uint16_t dst_device, std::uint16_t ctx_device,
                                std::uint8_t kind, std::uint8_t src_space, std::uint8_t dst_space,
                                std::uint8_t source, std::uint8_t async) {
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return;
    // Interval, always, for record_kernel's reason: [a, b] is the transfer's
    // window, and a source holding only one instant must say so through
    // `source` rather than by collapsing b onto a.
    tracer.recorder().record(pcomm::core::Temporal::interval({domain, t0}, {domain, t1}),
                             pcomm::core::records::CudaMemcpy{
                                 {},
                                 {},
                                 correlation,
                                 bytes,
                                 stream,
                                 src_device,
                                 dst_device,
                                 ctx_device,
                                 static_cast<pcomm::core::MemcpyKind>(kind),
                                 static_cast<pcomm::core::MemResidency>(src_space),
                                 static_cast<pcomm::core::MemResidency>(dst_space),
                                 static_cast<pcomm::core::CopySource>(source),
                                 async,
                             });
}

void pcomm_engine_clock_anchor(std::uint16_t domain, std::uint64_t local_ticks,
                               std::uint64_t wall_ns) {
    auto& tracer = pcomm::Tracer::instance();
    if (!tracer.has_recorder())
        return;
    // mono_ns is sampled here rather than taken from the caller: it is a host
    // clock, the caller is a GPU backend, and the only thing it is used for is
    // cross-checking the wall clock against a non-settable one.
    timespec mono{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &mono);
    const auto mono_ns = static_cast<std::uint64_t>(mono.tv_sec) * 1'000'000'000ull + mono.tv_nsec;
    tracer.recorder().record(
        pcomm::core::Temporal::none(),
        pcomm::core::records::ClockAnchor{{}, {}, domain, local_ticks, wall_ns, mono_ns});
}

// The user's knob, not the launcher's. Default ON, and the launcher no longer
// touches it: whether kernel lifetimes are wanted is a question about the run,
// not about which provider happens to be able to answer it. engine_abi.h
// carries what the previous conflation cost.
int pcomm_engine_kernel_timing_wanted(void) {
    const char* v = std::getenv("PCOMM_KERNEL_TIMING");
    if (v && (*v == '0' || *v == 'n' || *v == 'N'))
        return 0;
    return 1;
}

// Set by the launcher, which is the only thing that knows the provider list
// before any of them load. Default ON: a bare LD_PRELOAD chain with no gpumem
// in sight may take CUPTI. `pcomm run` clears it when gpumem is selected — see
// the arm-E measurement quoted in engine_abi.h for what happens if it does not.
int pcomm_engine_cupti_allowed(void) {
    const char* v = std::getenv("PCOMM_CUPTI_ALLOWED");
    if (v && (*v == '0' || *v == 'n' || *v == 'N'))
        return 0;
    return 1;
}

// How much to record. See pcomm::core::TraceMode for what the modes cost.
//
// FULL IS THE DEFAULT, and every fallback below lands on it too. The asymmetry
// that decides this: overhead is recoverable and data is not. A capture that
// cost 13 % can be taken again cheaply the moment you know you did not need the
// completions; a capture that silently omitted them cannot be taken again at
// all once the job, the allocation and the machine state are gone. On a system
// where a run means a queue slot, the expensive mistake is the missing stream,
// not the wasted percent.
//
// So an unrecognised value is still a LOUD default rather than a silent one —
// it says so on stderr — but it now defaults to recording MORE, not less.
// "mode=smple" should cost you time, never evidence.
int pcomm_engine_trace_mode(void) {
    static const int mode = [] {
        const char* v = std::getenv("PCOMM_TRACE_MODE");
        if (!v || !*v)
            return static_cast<int>(pcomm::core::TraceMode::Full);
        if (std::strcmp(v, "full") == 0 || std::strcmp(v, "2") == 0)
            return static_cast<int>(pcomm::core::TraceMode::Full);
        if (std::strcmp(v, "simple") == 0 || std::strcmp(v, "1") == 0)
            return static_cast<int>(pcomm::core::TraceMode::Simple);
        if (std::strcmp(v, "aggregate") == 0 || std::strcmp(v, "0") == 0) {
            std::fprintf(stderr, "pcomm: PCOMM_TRACE_MODE=aggregate is not implemented yet "
                                 "(it needs a counter record — see core/vocab.hpp); using full\n");
            return static_cast<int>(pcomm::core::TraceMode::Full);
        }
        std::fprintf(stderr, "pcomm: PCOMM_TRACE_MODE=%s is not a mode; using full\n", v);
        return static_cast<int>(pcomm::core::TraceMode::Full);
    }();
    return mode;
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

void pcomm_engine_note_cuda_live(const char* door) {
    pcomm::Tracer::instance().maybe_emit_gpu_devices(door ? door : "unnamed");
}

void pcomm_engine_arena_flush(pcomm_arena* arena) {
    if (arena && arena->_owner)
        static_cast<pcomm::Arena*>(arena->_owner)->drain();
    // The one in-run moment the engine reliably gets on any capture that
    // records anything. GpuDevice cannot be written at start (the application
    // may not have loaded CUDA yet) nor at finalize (the driver answers
    // CUDA_ERROR_DEINITIALIZED by then — job 45302619), so it is written the
    // first time a record buffer fills while the process is still alive.
    // A relaxed load on a path that has just done I/O.
    pcomm::Tracer::instance().maybe_emit_gpu_devices();
    // Invocation rides the same path for a DIFFERENT reason. It could be
    // written at start — nothing it reads changes — except for `providers`,
    // which is only complete once every preloaded .so has declared itself, and
    // for its interned handles, which the early flush cannot help anyway. Here
    // it is late enough to be complete and early enough to beat teardown.
    pcomm::maybe_emit_invocation();
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

// --- the intern hot path -----------------------------------------------------
//
// Providers intern once per RECORD, not once per entity: every send interns its
// endpoint pointer, every GPU access its object base. The call below is a
// per-domain mutex and an unordered_map probe, measured at 83.6 ns on this
// workstation and independently at 86 ns inside ucx_deep — against a 39 ns
// operation, so four fifths of that provider's cost was re-deriving a handle it
// had already been given.
//
// ucx_deep fixed it with a private cache in its own header. That left every
// other provider paying the toll and the next provider free to pay it again, so
// the cache lives here now and ucx_deep's is gone.
//
// **WHY A CACHE IS EXACT HERE, and not an approximation.** A (domain, key) ->
// handle mapping is stable for the life of the PROCESS. InternTable lives in a
// function-local static inside a Recorder member function, so every Recorder
// shares one set of tables; nothing anywhere clears them; and `atfork_child`
// RELEASES the parent's recorder rather than destroying it, so the child
// continues the same numbering rather than restarting it. The only way this can
// be wrong is by answering for the WRONG key, which the key comparison prevents
// and which tests/intern_hot_path.cpp drives with 4096 colliding keys.
//
// KEYS ONLY, NOT STRINGS. A string cache would have to be keyed on the bytes,
// which means hashing them — and hashing a string is most of what the table
// already does, so the cache would buy the mutex and little else. Interning a
// string is also rare by comparison: names are interned when an entity is
// created, keys on every event.
//
// PER THREAD, so there is no lock and no sharing. 128 slots x 5 domains x 16 B
// is 10 KB of .tbss per thread, which is the number to revisit if a stack ever
// runs hundreds of threads per rank. Direct-mapped rather than associative: a
// miss costs one comparison and falls through to the real call, so the cheapest
// possible probe is the right one.
namespace pcomm {
namespace {
constexpr std::size_t kInternCacheSlots = 128;  // power of two, see intern_bucket

struct InternCacheSlot {
    std::uint64_t key{0};
    std::uint32_t handle{0};
};

// Fibonacci hashing, then take the HIGH bits. Keys here are pointers (16-byte
// aligned, so the low bits carry nothing) and program counters (byte-addressed,
// so the low bits carry everything) — a plain shift is wrong for one or the
// other, and the multiply mixes both into the top.
inline std::size_t intern_bucket(std::uint64_t key) noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 57) & (kInternCacheSlots - 1);
}

thread_local InternCacheSlot t_intern_cache[pcomm::rec::kInternDomainCount][kInternCacheSlots];
}  // namespace
}  // namespace pcomm

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
    auto d = static_cast<pcomm::rec::InternDomain>(domain);

    // A domain out of range would index both the cache and the table out of
    // bounds. The table has always been indexed unchecked; adding a second
    // unchecked index is how one latent hazard becomes two, so it is checked
    // here for both. `domain` crosses a C ABI, which is exactly where a caller
    // built against a different header would send a value this build has no
    // slot for.
    if (domain >= pcomm::rec::kInternDomainCount)
        return 0;

    // The cache, and it is EXACT rather than approximate. See the note above it
    // for why that is a property of this codebase and not an assumption.
    pcomm::InternCacheSlot& slot = pcomm::t_intern_cache[domain][pcomm::intern_bucket(key)];
    if (slot.key == key && slot.handle != 0)
        return slot.handle;

    auto&           rec = tracer.recorder();
    std::lock_guard lock(rec.mutex(d));
    const auto      h = rec.table(d).intern(key);
    // Never cache the null handle: it is the "no recorder" answer, and storing
    // it would turn a transient state into a permanent wrong one.
    if (h != 0)
        slot = {key, h};
    return h;
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

// ---- Invocation (532) ------------------------------------------------------
//
// See core/records.hpp for what this record is for. It lives HERE rather than
// in tracer.hpp for two reasons that both come down to the same thing: the
// build id header is private to this target, and `declared()` is private to
// this file. Everything Invocation needs to answer "what was asked, and by
// which build" is already in this translation unit and nowhere else.

// The whole file, or "" — used for /proc/self/cmdline. Sized rather than
// stat'd because procfs reports st_size 0 for these.
std::string read_proc_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return {};
    std::string out;
    char        buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

// The traced command line, with the NUL separators rendered as spaces.
//
// This is the ONE fact a reader wants first and the capture never carried. It
// is the application's argv, not the launcher's: under mpirun each rank's
// cmdline is the target binary, which is why `env` below has to carry the
// mapping policy separately.
std::string traced_argv() {
    std::string raw = read_proc_file("/proc/self/cmdline");
    while (!raw.empty() && raw.back() == '\0')
        raw.pop_back();
    for (char& c : raw)
        if (c == '\0')
            c = ' ';
    return raw;
}

// The environment, filtered to an allowlist. See records.hpp: writing all of
// environ would put credentials and home paths in every capture, for the sake
// of the two variables anyone actually reads.
//
// The defaults are chosen to answer "why did the traffic look like that":
// SLURM_ and OMPI_MCA_rmaps_ carry the placement decision that made jobs
// 45329318 and 45330017 differ, and CUDA_VISIBLE_DEVICES is the remap that
// GpuDevice exists to survive — never once exercised in a capture, because
// nothing recorded whether it was set.
std::string captured_env() {
    static const char* const kDefault[] = {
        "PCOMM_", "SLURM_", "OMPI_MCA_rmaps_", "OMPI_COMM_WORLD_", "PMIX_RANK",
        "CUDA_VISIBLE_DEVICES", "LD_PRELOAD", "UCX_TLS", "NCCL_ALGO", "NCCL_PROTO",
    };
    std::vector<std::string> prefixes(std::begin(kDefault), std::end(kDefault));
    // Comma-separated additions, so a study can capture the variables ITS
    // question turns on without editing the engine.
    if (const char* extra = std::getenv("PCOMM_ENV_CAPTURE")) {
        std::string_view v{extra};
        while (!v.empty()) {
            const auto comma = v.find(',');
            auto       one = v.substr(0, comma);
            if (!one.empty())
                prefixes.emplace_back(one);
            if (comma == std::string_view::npos)
                break;
            v.remove_prefix(comma + 1);
        }
    }

    std::string out;
    for (char** e = environ; e && *e; ++e) {
        std::string_view kv{*e};
        for (const auto& p : prefixes) {
            if (kv.size() >= p.size() && kv.compare(0, p.size(), p) == 0) {
                out.append(kv);
                out.push_back('\n');
                break;
            }
        }
    }
    return out;
}

// Every mapped libpcomm object, with the size and mtime of the FILE ON DISK.
//
// This is the field that would have caught job 45331793 outright: the stale
// provider was a hand-made copy in a second directory, and its path and mtime
// say so immediately. Reads /proc/self/maps and keeps one line per distinct
// path whose basename starts with "libpcomm" or contains "_trace" — the two
// naming conventions this project's shared objects use.
std::string mapped_images() {
    const std::string      maps = read_proc_file("/proc/self/maps");
    std::vector<std::string> seen;
    std::string            out;
    std::size_t            pos = 0;
    while (pos < maps.size()) {
        const auto eol = maps.find('\n', pos);
        std::string_view line{maps.data() + pos, (eol == std::string::npos ? maps.size() : eol) - pos};
        pos = (eol == std::string::npos) ? maps.size() : eol + 1;

        // The path is the last field and starts at the first '/'; anonymous and
        // special mappings ([heap], [stack]) have none.
        const auto slash = line.find('/');
        if (slash == std::string_view::npos)
            continue;
        std::string path{line.substr(slash)};
        // A file replaced or deleted under a running process is shown as
        // "<path> (deleted)". That suffix used to reach the record verbatim, where
        // the space broke images_are_no_older_than_the_engine's parse and it
        // reported "no engine" for an engine that was mapped and merely replaced.
        // Stripped here — and NOT stat'd afterwards: the bare path now names the
        // replacement, and describing it would claim the wrong binary answered.
        constexpr std::string_view kDeleted = " (deleted)";
        const bool                 deleted =
            path.size() > kDeleted.size() &&
            path.compare(path.size() - kDeleted.size(), kDeleted.size(), kDeleted) == 0;
        if (deleted)
            path.resize(path.size() - kDeleted.size());
        const auto  base = path.rfind('/');
        std::string_view name{path};
        if (base != std::string::npos)
            name = std::string_view{path}.substr(base + 1);
        if (name.rfind("libpcomm", 0) != 0 && name.find("_trace") == std::string_view::npos)
            continue;
        bool dup = false;
        for (const auto& s : seen)
            if (s == path) {
                dup = true;
                break;
            }
        if (dup)
            continue;
        seen.push_back(path);

        struct stat st {};
        if (!deleted && ::stat(path.c_str(), &st) == 0) {
            char buf[64];
            std::snprintf(buf, sizeof buf, " %lld %lld", static_cast<long long>(st.st_size),
                          static_cast<long long>(st.st_mtime));
            out.append(path).append(buf).push_back('\n');
        } else {
            // Recorded anyway: a mapped object whose file is gone (deleted or
            // replaced under a running job) is itself the answer to "which
            // binary answered", and dropping it would hide exactly that.
            out.append(path).append(" ? ?").push_back('\n');
        }
    }
    return out;
}

std::atomic<bool> g_invocation_done{false};

}  // namespace

namespace pcomm {

// Once per process, from the arena-flush path. Same relaxed-atomic gate as
// GpuDevice and for the same reason: the only cost of two threads both passing
// it is a duplicate row, and this path has just done I/O.
void maybe_emit_invocation() {
    if (g_invocation_done.load(std::memory_order_relaxed))
        return;
    auto& tracer = Tracer::instance();
    if (!tracer.has_recorder())
        return;
    g_invocation_done.store(true, std::memory_order_relaxed);

    std::string provs;
    {
        std::lock_guard lock(declared_mutex());
        for (const auto& d : declared()) {
            if (!provs.empty())
                provs += ',';
            provs += d.name;
        }
    }

    // commit, dirty flag, build type, engine ABI and schema hash in one line.
    // The last two are here rather than left to the stream header because this
    // is the row a reader looks at when a capture will not parse.
    char build[192];
    std::snprintf(build, sizeof build, "commit=%s type=%s abi=%u schema=0x%016llX",
                  PCOMM_BUILD_COMMIT, PCOMM_BUILD_TYPE,
                  static_cast<unsigned>(PCOMM_ENGINE_ABI_VERSION),
                  static_cast<unsigned long long>(pcomm::rec::kSchemaHash));

    auto& rec = tracer.recorder();
    auto  s = [&rec](const std::string& v) {
        return v.empty() ? core::kNullHandle : rec.intern(rec::InternDomain::String, v);
    };
    rec.record(core::Temporal::none(),
               core::records::Invocation{{},
                                         {},
                                         s(traced_argv()),
                                         s(captured_env()),
                                         s(provs),
                                         s(build),
                                         s(mapped_images()),
                                         static_cast<std::uint32_t>(::getpid())});
}

}  // namespace pcomm

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

/* ── dying gracefully ───────────────────────────────────────────────────────
 *
 * A capture that ends in a signal used to keep only what had already been
 * flushed, and it failed in an INVERTED way: the high-volume streams survived
 * because they filled their buffers and drained incrementally, while the
 * one-row identity streams never drained at all. Quantum ESPRESSO kept
 * 18,700,773 ib_post_send rows and 2 of 4 node_identity rows, so 84% of its
 * sends landed on dst_rank = -1 -- and the BROKEN capture was the bigger one,
 * which defeats every heuristic anyone would use to notice it.
 *
 * THIS IS DELIBERATELY NOT ASYNC-SIGNAL-SAFE, and the choice is the design. A
 * correct handler may call almost nothing; flushing a capture means locks,
 * allocation and stdio. The alternative on offer is not "a safe handler" but
 * "no handler" -- guaranteed total loss of everything buffered. So it does the
 * unsafe thing on purpose, in a process that is already dying, with four guards
 * bounding what going wrong can cost:
 *
 *   1. ORDER. The cheap safe work is flushed to disk before the expensive risky
 *      work begins (see Tracer::flush_from_signal). A second fault in the risky
 *      half loses the dictionary and keeps the records.
 *   2. RECURSION GUARD. A fault inside the handler goes straight to the default
 *      disposition instead of recursing until the stack dies.
 *   3. NO WAITING. try_drain_all() rather than a blocking drain: the thread
 *      holding the registry lock may be the one that just faulted, and blocking
 *      would turn a crash into a hang, which under SLURM costs the rest of the
 *      walltime rather than one job.
 *   4. A DEADMAN. alarm() with SIGALRM restored to its terminating default, so
 *      a flush that wedges anywhere kills the process instead of parking it.
 *
 * IT LIVES HERE, NOT IN Tracer, for the reason engine_abi.h gives for owning
 * ibv_destroy_qp: a process-wide signal disposition belongs to no single
 * provider, and two providers must not install two handlers.
 *
 * AND IT INTERPOSES sigaction RATHER THAN MERELY CALLING IT. This is the part
 * that was learned the hard way. Installing at ELF-constructor time is not
 * enough: pcomm loads under LD_PRELOAD, so its handlers go in before MPI_Init,
 * and then OpenMPI installs its own over the top (libopen-pal imports
 * sigaction). Measured on Quantum ESPRESSO, job 45103790: the job exited 139
 * (SIGSEGV), ranks 1 and 3 finalized normally, and ranks 0 and 2 died with no
 * intern-blob.bin and 4 streams against 29 -- our handler never ran on them.
 * An earlier version of this code carefully declined to displace a handler the
 * application had already installed, which is the right instinct pointed the
 * wrong way: the displacement runs in the other direction, at a moment an
 * install-time check cannot see.
 *
 * So the application's own handler is kept, ours goes in front of it, and ours
 * chains to theirs once the flush is done. The application's semantics are
 * preserved exactly -- it still sees its own handler when it queries, and its
 * handler still runs with the same siginfo and context -- while ours survives
 * being overwritten.
 */
namespace {

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTERM, SIGINT};
/* Seconds the whole best-effort flush is allowed before the deadman fires.
 * Generous: a capture can be gigabytes, and the point is to bound a HANG. */
constexpr unsigned kFlushDeadmanSeconds = 30;

bool pcomm_is_fatal_signal(int sig) {
    for (int s : kFatalSignals)
        if (s == sig)
            return true;
    return false;
}

/* What the APPLICATION asked for, per signal — the disposition it believes is
 * installed. Plain arrays with no constructor: this is read from a signal
 * handler, and a std::map there is the use-after-destruction gpumem's
 * static_assert exists to forbid. NSIG is a compile-time bound on every Linux. */
struct AppAction {
    struct sigaction act;
    bool             set;
};
AppAction g_app_action[NSIG];

typedef int (*SigactionFn)(int, const struct sigaction*, struct sigaction*);
SigactionFn pcomm_real_sigaction() {
    static auto fn = pcomm_next_sym<SigactionFn>("sigaction");
    return fn;
}

/* Restore the default disposition WITHOUT going through our own interposer.
 *
 * Not fastidiousness -- a hang the first version of this code actually had.
 * ::signal(sig, SIG_DFL) from inside the handler is caught by the `signal` door
 * below, which dutifully records SIG_DFL as the application's wish and puts OUR
 * handler back in front of it. The ::raise() that follows then re-enters the
 * handler, hits the recursion guard, calls ::signal again, and loops until the
 * process is killed from outside. A door you can call by accident from inside
 * is a door that needs a bypass.
 */
void pcomm_restore_default(int sig) {
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    auto real = pcomm_real_sigaction();
    if (real)
        real(sig, &dfl, nullptr);
}

void pcomm_fatal_signal_handler(int sig, siginfo_t* info, void* ctx) {
    static volatile sig_atomic_t in_handler = 0;
    if (in_handler) { /* guard 2: we faulted while flushing. Give up cleanly. */
        pcomm_restore_default(sig);
        ::raise(sig);
        return;
    }
    in_handler = 1;

    pcomm_restore_default(SIGALRM); /* guard 4: its default terminates */
    ::alarm(kFlushDeadmanSeconds);
    pcomm::Tracer::instance().flush_from_signal();
    ::alarm(0);

    /* Then hand the signal on exactly as the application arranged for it. */
    const AppAction& app = g_app_action[sig];
    if (app.set) {
        if ((app.act.sa_flags & SA_SIGINFO) && app.act.sa_sigaction) {
            in_handler = 0; /* theirs may legitimately fault and handle it */
            app.act.sa_sigaction(sig, info, ctx);
            return;
        }
        if (app.act.sa_handler == SIG_IGN)
            return; /* they asked for it to be ignored; honour that */
        if (app.act.sa_handler && app.act.sa_handler != SIG_DFL) {
            in_handler = 0;
            app.act.sa_handler(sig);
            return;
        }
    }
    /* No application handler: die exactly as we would have without pcomm --
     * same signal, same status, same core dump. A profiler must not change how
     * the program it is watching appears to fail. */
    pcomm_restore_default(sig);
    ::raise(sig);
}

void pcomm_install_one(int sig) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = &pcomm_fatal_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    auto real = pcomm_real_sigaction();
    if (real)
        real(sig, &sa, nullptr);
}

}  // namespace

void pcomm_install_signal_handlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        /* An alternate stack, because the fault this most needs to survive is a
         * stack overflow, where there is no room to run a handler on the
         * faulting thread's own stack. A fixed 64 KiB rather than SIGSTKSZ:
         * since glibc 2.34 that expands to sysconf(_SC_SIGSTKSZ) and cannot
         * size a static array. Best effort -- if it cannot be had, the handlers
         * still install and still work for every other fault. */
        static char alt[65536];
        stack_t     ss;
        std::memset(&ss, 0, sizeof ss);
        ss.ss_sp = alt;
        ss.ss_size = sizeof alt;
        ::sigaltstack(&ss, nullptr);

        auto real = pcomm_real_sigaction();
        for (int sig : kFatalSignals) {
            /* Record whatever is already there, so a handler installed BEFORE
             * us is chained to rather than lost. */
            struct sigaction prev;
            std::memset(&prev, 0, sizeof prev);
            if (real && real(sig, nullptr, &prev) == 0 && prev.sa_handler != SIG_DFL) {
                g_app_action[sig].act = prev;
                g_app_action[sig].set = true;
            }
            pcomm_install_one(sig);
        }
    });
}

/* The interposer. Everything the application does to a signal we care about is
 * remembered and then re-fronted; everything else passes straight through. */
extern "C" int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    auto real = pcomm_real_sigaction();
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    if (sig <= 0 || sig >= NSIG || !pcomm_is_fatal_signal(sig))
        return real(sig, act, oldact);

    /* A QUERY must answer with the application's own view, never with ours --
     * otherwise a library that saves and restores a handler would save OURS and
     * reinstall it later as if it were its own. */
    if (oldact) {
        if (g_app_action[sig].set) {
            *oldact = g_app_action[sig].act;
        } else if (real(sig, nullptr, oldact) != 0) {
            return -1;
        } else if (oldact->sa_sigaction == &pcomm_fatal_signal_handler) {
            std::memset(oldact, 0, sizeof *oldact);
            oldact->sa_handler = SIG_DFL;
        }
    }
    if (!act)
        return 0; /* pure query, already answered */

    g_app_action[sig].act = *act;
    g_app_action[sig].set = true;
    pcomm_install_one(sig); /* ours goes back in front, whatever they asked for */
    return 0;
}

/* glibc's signal() does not route through the public sigaction, so it needs its
 * own door or a bare `signal(SIGSEGV, h)` would still displace us. */
extern "C" sighandler_t signal(int sig, sighandler_t handler) {
    if (sig <= 0 || sig >= NSIG || !pcomm_is_fatal_signal(sig)) {
        static auto real = pcomm_next_sym<sighandler_t (*)(int, sighandler_t)>("signal");
        return real ? real(sig, handler) : SIG_ERR;
    }
    sighandler_t     previous = g_app_action[sig].set ? g_app_action[sig].act.sa_handler : SIG_DFL;
    struct sigaction act;
    std::memset(&act, 0, sizeof act);
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    g_app_action[sig].act = act;
    g_app_action[sig].set = true;
    pcomm_install_one(sig);
    return previous;
}

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

/* The one dmabuf fd -> range table. See engine_abi.h for why it moved here from
 * a per-provider copy. A descriptor is only unique while open, so a later
 * conversion that reuses the number simply replaces the entry — the same rule
 * the per-provider tables had. */
namespace {
struct DmabufEntry {
    uint64_t base{0};
    uint64_t size{0};
    uint16_t device{0xFFFF};
};
struct DmabufTable {
    std::mutex                           mutex;
    std::unordered_map<int, DmabufEntry> by_fd;
    std::atomic<uint64_t>                per_door[4]{};
    std::atomic<uint64_t>                hits{0}, misses{0};
};
DmabufTable& dmabuf_table() {
    static DmabufTable* t = new DmabufTable;  // leaked: read from destructors
    return *t;
}
}  // namespace

extern "C" void pcomm_engine_dmabuf_note(int fd, uint64_t base, uint64_t size, uint16_t device,
                                         uint8_t door) {
    if (fd < 0)
        return;
    auto& t = dmabuf_table();
    {
        std::lock_guard<std::mutex> lk(t.mutex);
        t.by_fd[fd] = DmabufEntry{base, size, device};
    }
    if (door < 4)
        t.per_door[door].fetch_add(1, std::memory_order_relaxed);
}

extern "C" int pcomm_engine_dmabuf_find(int fd, uint64_t* base, uint64_t* size, uint16_t* device) {
    auto&       t = dmabuf_table();
    DmabufEntry e;
    {
        std::lock_guard<std::mutex> lk(t.mutex);
        auto                        it = t.by_fd.find(fd);
        if (it == t.by_fd.end()) {
            t.misses.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        e = it->second;
    }
    t.hits.fetch_add(1, std::memory_order_relaxed);
    if (base)
        *base = e.base;
    if (size)
        *size = e.size;
    if (device)
        *device = e.device;
    return 1;
}

extern "C" void pcomm_engine_dmabuf_stats(uint64_t* per_door4, uint64_t* hits, uint64_t* misses) {
    auto& t = dmabuf_table();
    if (per_door4)
        for (int i = 0; i < 4; ++i)
            per_door4[i] = t.per_door[i].load(std::memory_order_relaxed);
    if (hits)
        *hits = t.hits.load(std::memory_order_relaxed);
    if (misses)
        *misses = t.misses.load(std::memory_order_relaxed);
}

/* A provider recorded a queue-pair open; remember the handle so the engine's
 * ibv_destroy_qp closes THIS queue pair knowingly, and reports the door that
 * created it. See QpRegistry and pcomm_close_qp for what the two paths mean. */
extern "C" void pcomm_engine_note_qp(void* qp, uint16_t opener_type_id) {
    if (!qp || !opener_type_id)
        return;
    QpRegistry::instance().add(reinterpret_cast<std::uint64_t>(qp), opener_type_id);
}
