// ucx_deep — UCX's TRANSPORT layer, through the one door it does have.
//
// THE PROBLEM THIS SOLVES. The 2026-08-27 door sweep (PLAN-20260815-1540,
// Future work 1) measured that on the HPC-X/mlx5 stack `ib_post_send` and
// `mlx5_device_send` are BOTH empty against 24,452 tagged sends, and concluded
// that UCX "writes WQEs and rings the UAR inline, with no interposable symbol".
// That is right about symbols and wrong about doors.
//
// `uct/api/uct.h` declares every UCT data operation as UCT_INLINE_API:
//
//     UCT_INLINE_API ucs_status_t uct_ep_am_short(uct_ep_h ep, ...)
//     { return ep->iface->ops.ep_am_short(ep, ...); }
//
// so there is no `uct_ep_am_short` in anybody's PLT to find. The dispatch goes
// through `uct_iface_ops_t`, and BOTH structs on the path to it are declared in
// `uct/api/tl.h`, which UCX INSTALLS:
//
//     typedef struct uct_iface { uct_iface_ops_t ops; }   uct_iface_t;
//     typedef struct uct_ep    { uct_iface_h     iface; } uct_ep_t;
//
// No offset is guessed and no pointer is walked — the compiler computes every
// offset from a header the runtime ships. tl.h's own doc comment sanctions
// this: "Every operation exposed in the API must appear in the table below, to
// allow creating interface/endpoint with custom operations."
//
// WHAT IS STILL RISKY, AND WHAT PAYS FOR IT. tl.h is the transport-implementor
// half of UCX's API and carries no ABI promise. Measured rather than assumed:
// `src/uct/api/tl.h` is BYTE-IDENTICAL (md5 bef986ac) from v1.14.0 through
// v1.19.1 and origin/master, and the copy MN5's HPC-X 2.22.1 installs is
// bit-identical to upstream. Six minor releases; the last change was a
// copyright line. Three guards pay for the remaining risk — a runtime version
// range, a structural check on the table itself, and a `uct_binding` record so
// the verdict is in the capture instead of in this comment.
//
// WHY NOT ObjectOpsHookManager, which exists for exactly this shape of door.
// Its slot table is 1024 entries GLOBAL, consumed one per (tag, object), and it
// instantiates one trampoline per (tag, slot). Fourteen data ops would mean
// 14,336 generated functions and a ceiling of 73 interfaces per process — and
// UCX opens an interface per (transport, device) per worker, which MN5 job
// 44369505 already measured at ~20 ibv_contexts per rank for the same reason.
// Being within 4x of a silent-degradation ceiling is not a margin. So the
// trampolines here are hand-written against one shadow table with no ceiling
// but its own, and the lookup cost is measured rather than assumed — see
// find_shadow().

#include <dlfcn.h>

#include <cerrno>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>

#include <ucp/api/ucp.h>
#include <uct/api/uct.h>
#include <uct/api/version.h>

#include "pcomm/core/records.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/tracer.hpp"

#include "ucx_deep_event_source.hpp"
#include "ucx_deep_records.hpp"
#include "ucx_deep_tables.hpp"

namespace ucx_deep {
namespace rr = pcomm::providers::ucx_deep::records;
namespace detail = pcomm::providers::ucx_deep::detail;
using pcomm::rec::InternDomain;

static pcomm::rec::Recorder<>& rec() {
    static pcomm::rec::Recorder<> proxy;
    return proxy;
}

static pcomm::core::Handle h(const void* p) {
    return rec().intern(InternDomain::Pointer, reinterpret_cast<std::uint64_t>(p));
}

// THE HOT PATH'S ONE OPTIMISATION, and it is here because it was measured, not
// because it looked slow.
//
// Interning is not a hash lookup: rec().intern() is a call across the engine's
// C ABI into a table behind a per-domain mutex. Measured on the self transport,
// 200,000 tagged sends, one uct_send row each:
//
//     bare UCX                                   13.9 ns/op
//     + trampoline and timestamps, no record      25   ns/op
//     + the record, endpoint handle not interned  53   ns/op
//     + interning the endpoint pointer           138   ns/op   <-- 86 ns of it
//
// So four fifths of this provider's cost was one call. A UCT endpoint pointer's
// handle is immutable — pcomm_engine_intern_key is keyed by the raw address and
// returns the same handle forever, even after the address is freed and re-used
// — so caching it is not an approximation, and a stale entry cannot exist.
//
// Direct-mapped and per-thread: a lane is used by one thread at a time and a
// burst of sends hits one lane, so 16 entries make this a compare in the case
// that matters while a multi-lane endpoint still degrades to the real call
// rather than to a wrong answer.
static pcomm::core::Handle ep_handle(const void* p) noexcept {
    static thread_local detail::HandleCache<16> cache;
    if (const auto hit = cache.lookup(p); hit != pcomm::core::kNullHandle) [[likely]]
        return hit;
    const auto handle = h(p);
    cache.store(p, handle);
    return handle;
}

static pcomm::core::Temporal span(std::uint64_t begin, std::uint64_t end) {
    return pcomm::core::Temporal::interval({pcomm::core::kDomainHostTsc, begin},
                                           {pcomm::core::kDomainHostTsc, end});
}

static std::uint64_t now() noexcept {
    return pcomm::helpers::ts::RdtscTimestampSource::Now();
}

static void close_entity(const void* p, pcomm::core::RecordTypeId opener) {
    if (p)
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{{}, {}, h(p), opener});
}

// ===========================================================================
// GUARD 1 — the UCX we were compiled against, against the one that loaded.
// ===========================================================================
//
// Refusing is the correct failure mode and it is not a close call. A capture
// that is empty because we refused costs a re-run; a trampoline written into
// the wrong slot of a live transport calls the wrong function with the wrong
// arguments and takes the APPLICATION down. The asymmetry is total, so the
// range is the one that was actually verified identical and nothing wider.
//
// The range is verified by md5 of `src/uct/api/tl.h`, the header that declares
// uct_iface_ops_t, taken from the release tags themselves:
//
//   v1.14.0  bef986ac      v1.19.1        bef986ac
//   v1.18.0  bef986ac      v1.20.0        bef986ac   <- added 2026-09-02
//   v1.19.0  bef986ac      origin/master  bef986ac
//
// 20 was added because a capture demanded it and the evidence allowed it, in
// that order. The 02_lammps round-2 study (jobs 45329318 and 45330017) ran on
// MN5's UCX 1.20.0, GUARD 1 refused exactly as designed, and `transport_mix`
// came back empty — so the second half of that study's prediction, that a scope
// derived from tl_name agrees with one derived from node membership, could not
// be answered at all.
//
// The refusal was right and the range was merely stale: master already carried
// the same md5 when 19 was the ceiling, so 1.20.0 was always going to match. It
// is widened here only because that was checked against the tag rather than
// assumed from master.
static constexpr unsigned kMinVerifiedMinor = 14;  // v1.14.0 — tl.h md5 bef986ac
static constexpr unsigned kMaxVerifiedMinor = 20;  // v1.20.0 and origin/master, same md5

static std::uint32_t pack_version(unsigned major, unsigned minor, unsigned patch) noexcept {
    return (major << 16) | ((minor & 0xff) << 8) | (patch & 0xff);
}

// ===========================================================================
// GUARD 2 — the table itself, checked without reference to any version number.
// ===========================================================================
//
// UCS_CLASS_INIT_FUNC(uct_iface_t) (src/uct/base/uct_iface.c) asserts twelve
// slots non-NULL before it copies the table. So UCX itself guarantees what we
// can test: if any of those twelve reads NULL, or reads something that is not
// executable code belonging to a UCX object, then the struct we compiled
// against is not the struct in front of us and every other slot index is a
// guess.
//
// Executable-mapping membership, not dladdr(): dladdr resolves a DATA pointer
// to its object just as happily as a function pointer, so a shifted layout that
// happened to put a `ucs_config_field` array in one of these slots would sail
// through a dladdr check. Executable ranges out of /proc/self/maps do not have
// that hole.
//
// KNOWN LIMIT: an application with UCX linked STATICALLY has its transports in
// the executable, not in a libuc* object, so this refuses. That is a false
// negative, it is safe, and it announces itself — which is the whole contract.
struct CodeRange {
    std::uintptr_t lo, hi;
};

// Re-readable, and it has to be: UCX dlopens its transport components
// (libuct_ib, libuct_cuda, libuct_cma) as it discovers devices, so a snapshot
// taken at the first uct_iface_open does not contain the object that provides
// the SECOND one. A stale map makes the guard refuse an interface it should
// have accepted — safe, but it loses capture for no reason — so a failed check
// refreshes once before it refuses. See ops_layout_plausible's caller.
static std::vector<CodeRange>& ucx_code_ranges_storage() {
    static auto* v = new std::vector<CodeRange>();
    return *v;
}

static void refresh_ucx_code_ranges() {
    auto* v = &ucx_code_ranges_storage();
    v->clear();
    std::FILE* f = std::fopen("/proc/self/maps", "re");
    if (!f) {
        std::fprintf(stderr,
                     "[pcomm] ucx_deep: cannot read /proc/self/maps (%s); the UCT ops table "
                     "cannot be validated and will NOT be patched\n",
                     std::strerror(errno));
        std::fflush(stderr);
        return;
    }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long lo = 0, hi = 0;
        char               perms[8] = {0};
        int                path_off = 0;
        if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &lo, &hi, perms, &path_off) < 3)
            continue;
        if (perms[2] != 'x' || path_off <= 0)
            continue;
        const char* path = line + path_off;
        const char* base = std::strrchr(path, '/');
        base = base ? base + 1 : path;
        if (std::strncmp(base, "libuc", 5) == 0)
            v->push_back({static_cast<std::uintptr_t>(lo), static_cast<std::uintptr_t>(hi)});
    }
    std::fclose(f);
}

static const std::vector<CodeRange>& ucx_code_ranges() {
    static bool once = (refresh_ucx_code_ranges(), true);
    (void)once;
    return ucx_code_ranges_storage();
}

static bool is_ucx_code(const void* p) noexcept {
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    for (const auto& r : ucx_code_ranges())
        if (a >= r.lo && a < r.hi)
            return true;
    return false;
}

// The exact twelve UCX asserts on. Named individually rather than looped,
// because the point is that these are the slots UCX ITSELF promises are filled
// — a loop over "every non-NULL slot" would test a different, weaker claim.
static bool ops_layout_plausible(const uct_iface_ops_t& ops, const char** which) noexcept {
    const struct {
        const char* name;
        const void* fn;
    } mandatory[] = {
        {"ep_flush", reinterpret_cast<const void*>(ops.ep_flush)},
        {"ep_fence", reinterpret_cast<const void*>(ops.ep_fence)},
        {"ep_destroy", reinterpret_cast<const void*>(ops.ep_destroy)},
        {"iface_flush", reinterpret_cast<const void*>(ops.iface_flush)},
        {"iface_fence", reinterpret_cast<const void*>(ops.iface_fence)},
        {"iface_progress_enable", reinterpret_cast<const void*>(ops.iface_progress_enable)},
        {"iface_progress_disable", reinterpret_cast<const void*>(ops.iface_progress_disable)},
        {"iface_progress", reinterpret_cast<const void*>(ops.iface_progress)},
        {"iface_close", reinterpret_cast<const void*>(ops.iface_close)},
        {"iface_query", reinterpret_cast<const void*>(ops.iface_query)},
        {"iface_get_device_address", reinterpret_cast<const void*>(ops.iface_get_device_address)},
        {"iface_is_reachable", reinterpret_cast<const void*>(ops.iface_is_reachable)},
    };
    for (const auto& m : mandatory) {
        if (m.fn == nullptr || !is_ucx_code(m.fn)) {
            *which = m.name;
            return false;
        }
    }
    return true;
}

// Decided once. `accepted` gates every patch; nothing else reads the versions.
struct Binding {
    bool          accepted{false};
    bool          forced{false};
    std::uint32_t runtime{0};
};

static const Binding& binding() {
    static const Binding b = [] {
        Binding  out;
        unsigned maj = 0, min = 0, patch = 0;
        ucp_get_version(&maj, &min, &patch);
        out.runtime = pack_version(maj, min, patch);
        const bool  in_range = (maj == 1 && min >= kMinVerifiedMinor && min <= kMaxVerifiedMinor);
        const char* force = std::getenv("PCOMM_UCX_DEEP_FORCE");
        out.forced = force && *force && std::strcmp(force, "0") != 0;
        out.accepted = in_range || out.forced;
        if (!in_range && out.forced) {
            std::fprintf(
                stderr,
                "[pcomm] ucx_deep: UCX %u.%u.%u is OUTSIDE the range whose uct_iface_ops_t "
                "layout was verified (1.%u - 1.%u), and PCOMM_UCX_DEEP_FORCE is set. "
                "Patching anyway. If the ops table moved, this WILL corrupt the "
                "application, not merely the capture.\n",
                maj, min, patch, kMinVerifiedMinor, kMaxVerifiedMinor);
        } else if (!in_range) {
            std::fprintf(stderr,
                         "[pcomm] ucx_deep: REFUSING to instrument. UCX %u.%u.%u is outside the "
                         "range whose uct_iface_ops_t layout was verified (1.%u - 1.%u). "
                         "uct_send will be EMPTY — this is a refusal, not an absence of traffic. "
                         "Re-verify tl.h against this version and widen the range, or set "
                         "PCOMM_UCX_DEEP_FORCE=1 to override.\n",
                         maj, min, patch, kMinVerifiedMinor, kMaxVerifiedMinor);
        }
        std::fflush(stderr);
        return out;
    }();
    return b;
}

// ===========================================================================
// The shadow table.
// ===========================================================================
//
// One entry per instrumented interface, holding the ops table as it was before
// we touched it. Every trampoline needs it, so this lookup is THE hot-path cost
// of the whole provider and is measured in tests/ucx_deep_records.sh.
//
// NEVER REMOVED, and that is deliberate rather than lazy: a trampoline can be
// entered concurrently with iface_close (another thread mid-send), and an entry
// erased out from under it would be a use-after-free inside the application. An
// interface closed and a new one opened at the same address re-uses the entry,
// because insert() probes for the pointer first — so pointer re-use, which is
// what makes stale entity state dangerous everywhere else in pcomm, is handled
// by overwrite rather than by deletion.
//
// LEAKED ON PURPOSE. Same reasoning as ucx_provider.cpp's ep_open(): UCX can
// close interfaces from an atexit handler, which runs after this translation
// unit's namespace-scope destructors, and reading a destroyed container there
// is the bug that made every gpumem case segfault at 0x8.
static constexpr std::size_t kMaxIfaces = 256;
static constexpr std::size_t kSlots = 1024;

struct Shadow {
    // `key` rather than `iface`: detail::PointerTable keys on this member, and
    // the test that drives the collision behaviour must not need a UCX header.
    const void*         key{nullptr};
    uct_iface_ops_t     orig{};
    pcomm::core::Handle iface_h{pcomm::core::kNullHandle};
};

using ShadowTable = detail::PointerTable<Shadow, kMaxIfaces, kSlots>;

// LEAKED ON PURPOSE. Same reasoning as ucx_provider.cpp's ep_open(): UCX can
// close interfaces from an atexit handler, which runs after this translation
// unit's namespace-scope destructors, and reading a destroyed object there is
// the bug that made every gpumem case segfault at 0x8.
static ShadowTable& shadows() {
    static auto* t = new ShadowTable();
    return *t;
}

static std::mutex& table_mu() {
    static std::mutex* m = new std::mutex();
    return *m;
}

// Lock-free. Correct against a concurrent insert because a slot only ever goes
// null -> non-null; see PointerTable.
static Shadow* find_shadow(uct_iface_h iface) noexcept {
    // Most sends in a burst go to the same interface, so this one-entry cache
    // is what keeps the common case at a compare rather than a probe.
    static thread_local uct_iface_h last_iface = nullptr;
    static thread_local Shadow*     last_shadow = nullptr;
    if (last_iface == iface && last_shadow)
        return last_shadow;
    Shadow* s = shadows().find(iface);
    if (s) {
        last_iface = iface;
        last_shadow = s;
    }
    return s;
}

// A trampoline that cannot find its shadow has no original to call, so it
// cannot fall through. It is structurally impossible — the shadow is inserted
// and filled BEFORE any slot is patched, and entries are never removed — but
// "impossible" has been wrong here before, so it says so and returns a real UCX
// error rather than dereferencing null.
static Shadow* need_shadow(uct_iface_h iface) noexcept {
    Shadow* s = find_shadow(iface);
    if (s) [[likely]]
        return s;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        std::fprintf(stderr,
                     "[pcomm] ucx_deep: BUG — a patched uct_iface_ops slot fired for interface %p "
                     "with no shadow entry. Operations on it are being FAILED, not passed through, "
                     "because the original function pointer is unrecoverable.\n",
                     static_cast<void*>(iface));
        std::fflush(stderr);
    }
    return nullptr;
}

// ===========================================================================
// Emitting
// ===========================================================================
static void emit(Shadow* s, uct_ep_h ep, rr::UctOp op, std::uint64_t payload, std::uint64_t header,
                 std::uint64_t remote_addr, std::uint64_t tag, std::uint8_t iovcnt,
                 std::int32_t status, std::uint64_t begin, std::uint64_t end) {
    rec().record(span(begin, end), rr::UctSend{{},
                                               {},
                                               payload,
                                               header,
                                               remote_addr,
                                               tag,
                                               ep_handle(ep),
                                               s->iface_h,
                                               status,
                                               static_cast<std::uint8_t>(op),
                                               iovcnt,
                                               0,
                                               0});
}

// The three length rules live in ucx_deep_tables.hpp so tests/ucx_deep_tables.cpp
// can drive them directly. The local workload cannot: over the self transport
// nothing fails, nothing strides and nothing scatters, so a mutation that
// hardcodes success or drops the stride multiply survives the record gate.
using detail::BcopyResult;
using detail::classify_bcopy;
using detail::iov_bytes;
using detail::saturate_iovcnt;
static_assert(static_cast<int>(UCS_OK) == 0,
              "classify_bcopy returns a literal 0 for UCS_OK so the header needs no UCX");

// ===========================================================================
// The trampolines. Fourteen data ops, plus the three lifecycle slots.
// ===========================================================================
#define UCTD_ENTER(_ep)                                                                            \
    Shadow* s__ = need_shadow((_ep)->iface);                                                       \
    if (!s__) [[unlikely]]                                                                         \
        return UCS_ERR_IO_ERROR;                                                                   \
    const std::uint64_t t0__ = now()

static ucs_status_t tr_am_short(uct_ep_h ep, uint8_t id, uint64_t header, const void* payload,
                                unsigned length) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_am_short(ep, id, header, payload, length);
    emit(s__, ep, rr::UctOp::AmShort, length, sizeof(header), 0, 0, 0, st, t0__, now());
    return st;
}

static ucs_status_t tr_am_short_iov(uct_ep_h ep, uint8_t id, const uct_iov_t* iov, size_t iovcnt) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_am_short_iov(ep, id, iov, iovcnt);
    emit(s__, ep, rr::UctOp::AmShortIov, iov_bytes(iov, iovcnt), 0, 0, 0, saturate_iovcnt(iovcnt),
         st, t0__, now());
    return st;
}

static ssize_t tr_am_bcopy(uct_ep_h ep, uint8_t id, uct_pack_callback_t pack_cb, void* arg,
                           unsigned flags) {
    Shadow* s = need_shadow(ep->iface);
    if (!s) [[unlikely]]
        return UCS_ERR_IO_ERROR;
    const std::uint64_t t0 = now();
    const ssize_t       ret = s->orig.ep_am_bcopy(ep, id, pack_cb, arg, flags);
    const auto          r = classify_bcopy(ret);
    emit(s, ep, rr::UctOp::AmBcopy, r.payload, 0, 0, 0, 0, r.status, t0, now());
    return ret;
}

static ucs_status_t tr_am_zcopy(uct_ep_h ep, uint8_t id, const void* header, unsigned header_length,
                                const uct_iov_t* iov, size_t iovcnt, unsigned flags,
                                uct_completion_t* comp) {
    UCTD_ENTER(ep);
    const ucs_status_t st =
        s__->orig.ep_am_zcopy(ep, id, header, header_length, iov, iovcnt, flags, comp);
    emit(s__, ep, rr::UctOp::AmZcopy, iov_bytes(iov, iovcnt), header_length, 0, 0,
         saturate_iovcnt(iovcnt), st, t0__, now());
    return st;
}

static ucs_status_t tr_put_short(uct_ep_h ep, const void* buffer, unsigned length,
                                 uint64_t remote_addr, uct_rkey_t rkey) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_put_short(ep, buffer, length, remote_addr, rkey);
    emit(s__, ep, rr::UctOp::PutShort, length, 0, remote_addr, 0, 0, st, t0__, now());
    return st;
}

static ssize_t tr_put_bcopy(uct_ep_h ep, uct_pack_callback_t pack_cb, void* arg,
                            uint64_t remote_addr, uct_rkey_t rkey) {
    Shadow* s = need_shadow(ep->iface);
    if (!s) [[unlikely]]
        return UCS_ERR_IO_ERROR;
    const std::uint64_t t0 = now();
    const ssize_t       ret = s->orig.ep_put_bcopy(ep, pack_cb, arg, remote_addr, rkey);
    const auto          r = classify_bcopy(ret);
    emit(s, ep, rr::UctOp::PutBcopy, r.payload, 0, remote_addr, 0, 0, r.status, t0, now());
    return ret;
}

static ucs_status_t tr_put_zcopy(uct_ep_h ep, const uct_iov_t* iov, size_t iovcnt,
                                 uint64_t remote_addr, uct_rkey_t rkey, uct_completion_t* comp) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_put_zcopy(ep, iov, iovcnt, remote_addr, rkey, comp);
    emit(s__, ep, rr::UctOp::PutZcopy, iov_bytes(iov, iovcnt), 0, remote_addr, 0,
         saturate_iovcnt(iovcnt), st, t0__, now());
    return st;
}

static ucs_status_t tr_get_short(uct_ep_h ep, void* buffer, unsigned length, uint64_t remote_addr,
                                 uct_rkey_t rkey) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_get_short(ep, buffer, length, remote_addr, rkey);
    emit(s__, ep, rr::UctOp::GetShort, length, 0, remote_addr, 0, 0, st, t0__, now());
    return st;
}

static ucs_status_t tr_get_bcopy(uct_ep_h ep, uct_unpack_callback_t unpack_cb, void* arg,
                                 size_t length, uint64_t remote_addr, uct_rkey_t rkey,
                                 uct_completion_t* comp) {
    UCTD_ENTER(ep);
    const ucs_status_t st =
        s__->orig.ep_get_bcopy(ep, unpack_cb, arg, length, remote_addr, rkey, comp);
    // REQUESTED, not delivered — the data arrives later, into `arg`. See UctOp.
    emit(s__, ep, rr::UctOp::GetBcopy, length, 0, remote_addr, 0, 0, st, t0__, now());
    return st;
}

static ucs_status_t tr_get_zcopy(uct_ep_h ep, const uct_iov_t* iov, size_t iovcnt,
                                 uint64_t remote_addr, uct_rkey_t rkey, uct_completion_t* comp) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_get_zcopy(ep, iov, iovcnt, remote_addr, rkey, comp);
    emit(s__, ep, rr::UctOp::GetZcopy, iov_bytes(iov, iovcnt), 0, remote_addr, 0,
         saturate_iovcnt(iovcnt), st, t0__, now());
    return st;
}

static ucs_status_t tr_tag_eager_short(uct_ep_h ep, uct_tag_t tag, const void* data,
                                       size_t length) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_tag_eager_short(ep, tag, data, length);
    emit(s__, ep, rr::UctOp::TagEagerShort, length, 0, 0, tag, 0, st, t0__, now());
    return st;
}

static ssize_t tr_tag_eager_bcopy(uct_ep_h ep, uct_tag_t tag, uint64_t imm,
                                  uct_pack_callback_t pack_cb, void* arg, unsigned flags) {
    Shadow* s = need_shadow(ep->iface);
    if (!s) [[unlikely]]
        return UCS_ERR_IO_ERROR;
    const std::uint64_t t0 = now();
    const ssize_t       ret = s->orig.ep_tag_eager_bcopy(ep, tag, imm, pack_cb, arg, flags);
    const auto          r = classify_bcopy(ret);
    emit(s, ep, rr::UctOp::TagEagerBcopy, r.payload, 0, 0, tag, 0, r.status, t0, now());
    return ret;
}

static ucs_status_t tr_tag_eager_zcopy(uct_ep_h ep, uct_tag_t tag, uint64_t imm,
                                       const uct_iov_t* iov, size_t iovcnt, unsigned flags,
                                       uct_completion_t* comp) {
    UCTD_ENTER(ep);
    const ucs_status_t st = s__->orig.ep_tag_eager_zcopy(ep, tag, imm, iov, iovcnt, flags, comp);
    emit(s__, ep, rr::UctOp::TagEagerZcopy, iov_bytes(iov, iovcnt), 0, 0, tag,
         saturate_iovcnt(iovcnt), st, t0__, now());
    return st;
}

static ucs_status_ptr_t tr_tag_rndv_zcopy(uct_ep_h ep, uct_tag_t tag, const void* header,
                                          unsigned header_length, const uct_iov_t* iov,
                                          size_t iovcnt, unsigned flags, uct_completion_t* comp) {
    Shadow* s = need_shadow(ep->iface);
    if (!s) [[unlikely]]
        return UCS_STATUS_PTR(UCS_ERR_IO_ERROR);
    const std::uint64_t    t0 = now();
    const ucs_status_ptr_t ret =
        s->orig.ep_tag_rndv_zcopy(ep, tag, header, header_length, iov, iovcnt, flags, comp);
    // ADVERTISED, not sent: only `header_length` bytes leave now; the peer pulls
    // the iov later. Summing `payload` across families would count these twice.
    emit(s, ep, rr::UctOp::TagRndvZcopy, iov_bytes(iov, iovcnt), header_length, 0, tag,
         saturate_iovcnt(iovcnt),
         UCS_PTR_IS_ERR(ret) ? static_cast<std::int32_t>(UCS_PTR_STATUS(ret)) : UCS_OK, t0, now());
    return ret;
}

// --- lifecycle slots -------------------------------------------------------
//
// ep_create and ep_destroy are hooked HERE rather than at the exported
// uct_ep_create/uct_ep_destroy symbols on purpose. Both of those dispatch
// straight through these slots (src/uct/base/uct_iface.c:726), and a transport
// can create endpoints of its own that never pass the public symbol — DC's
// internal endpoints among them. Hooking the slot catches the public call AND
// the internal one; hooking the symbol catches only the first, and a uct_send
// naming an endpoint nothing created is the "entity referenced by events that
// nothing opened" defect ib_hooks.hpp spent a week on.
static ucs_status_t tr_ep_create(const uct_ep_params_t* params, uct_ep_h* ep_p) {
    if (!params || !(params->field_mask & UCT_EP_PARAM_FIELD_IFACE) || !params->iface)
        return UCS_ERR_INVALID_PARAM;  // unreachable: uct only dispatches here via params->iface
    Shadow* s = need_shadow(params->iface);
    if (!s) [[unlikely]]
        return UCS_ERR_IO_ERROR;
    const std::uint64_t begin = now();
    const ucs_status_t  st = s->orig.ep_create(params, ep_p);
    const std::uint64_t end = now();
    if (st == UCS_OK && ep_p && *ep_p)
        rec().record(span(begin, end), rr::UctEndpoint{{}, {}, h(*ep_p), s->iface_h});
    return st;
}

static void tr_ep_destroy(uct_ep_h ep) {
    Shadow* s = need_shadow(ep->iface);
    if (!s) [[unlikely]]
        return;  // nothing to call; leaking one endpoint beats calling null
    close_entity(ep, pcomm::core::RecordTrait<rr::UctEndpoint>::type_id);
    s->orig.ep_destroy(ep);
}

static void tr_iface_close(uct_iface_h iface) {
    Shadow* s = need_shadow(iface);
    if (!s) [[unlikely]]
        return;
    close_entity(iface, pcomm::core::RecordTrait<rr::UctIface>::type_id);
    // The shadow entry deliberately OUTLIVES the interface — see the table's
    // comment. Another thread can still be inside a trampoline for this
    // interface right now, and erasing its original function pointer would turn
    // our instrumentation into a crash in the application.
    s->orig.iface_close(iface);
}

// ===========================================================================
// Patching
// ===========================================================================
static std::uint8_t patch(Shadow* s, uct_iface_ops_t* ops) {
    std::uint8_t n = 0;
    // A NULL slot is NOT patched. ib_event_source.hpp's vctx_slot spells out
    // why: a hook must never manufacture a capability the application did not
    // have. UCX gates these on iface_attr.cap.flags rather than on the pointer,
    // so patching a NULL would not fabricate a capability here — but it would
    // replace "the transport crashes on an op it never advertised" with "the
    // TRACER crashes on it", which is strictly worse and harder to read.
#define UCTD_PATCH(_slot, _fn)                                                                     \
    if (ops->_slot) {                                                                              \
        ops->_slot = (_fn);                                                                        \
        ++n;                                                                                       \
    }
    UCTD_PATCH(ep_am_short, tr_am_short)
    UCTD_PATCH(ep_am_short_iov, tr_am_short_iov)
    UCTD_PATCH(ep_am_bcopy, tr_am_bcopy)
    UCTD_PATCH(ep_am_zcopy, tr_am_zcopy)
    UCTD_PATCH(ep_put_short, tr_put_short)
    UCTD_PATCH(ep_put_bcopy, tr_put_bcopy)
    UCTD_PATCH(ep_put_zcopy, tr_put_zcopy)
    UCTD_PATCH(ep_get_short, tr_get_short)
    UCTD_PATCH(ep_get_bcopy, tr_get_bcopy)
    UCTD_PATCH(ep_get_zcopy, tr_get_zcopy)
    UCTD_PATCH(ep_tag_eager_short, tr_tag_eager_short)
    UCTD_PATCH(ep_tag_eager_bcopy, tr_tag_eager_bcopy)
    UCTD_PATCH(ep_tag_eager_zcopy, tr_tag_eager_zcopy)
    UCTD_PATCH(ep_tag_rndv_zcopy, tr_tag_rndv_zcopy)
#undef UCTD_PATCH
    // Lifecycle slots are patched unconditionally-if-present and do NOT count
    // towards patched_ops, which is a count of DATA slots — the number a reader
    // uses to decide whether an empty uct_send for this interface is a fact
    // about the workload or about the transport.
    if (ops->ep_create)
        ops->ep_create = tr_ep_create;
    if (ops->ep_destroy)
        ops->ep_destroy = tr_ep_destroy;
    if (ops->iface_close)
        ops->iface_close = tr_iface_close;
    (void)s;
    return n;
}

static void on_iface_open(uct_md_h md, uct_worker_h worker, const uct_iface_params_t* params,
                          uct_iface_h iface, std::uint64_t begin, std::uint64_t end) {
    pcomm::core::Handle tl_h = pcomm::core::kNullHandle;
    pcomm::core::Handle dev_h = pcomm::core::kNullHandle;
    std::uint64_t       open_mode = 0;

    // EVERY optional field through its mask, for the reason ucp_worker_query
    // needed it: uct_iface_params_t is a masked struct, and a client-server
    // interface is opened by socket address with mode.device holding whatever
    // the caller's stack did. Reading it regardless would put a random pointer
    // through strlen.
    if (params) {
        if (params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE)
            open_mode = params->open_mode;
        if ((params->field_mask & UCT_IFACE_PARAM_FIELD_DEVICE) &&
            (open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
            if (params->mode.device.tl_name)
                tl_h = rec().intern(InternDomain::String, params->mode.device.tl_name);
            if (params->mode.device.dev_name)
                dev_h = rec().intern(InternDomain::String, params->mode.device.dev_name);
        }
    }

    auto                instrumented = rr::UctInstrumented::No;
    std::uint8_t        patched = 0;
    pcomm::core::Handle iface_h = h(iface);

    if (binding().accepted) {
        const char* bad = nullptr;
        bool        ok = ops_layout_plausible(iface->ops, &bad);
        if (!ok) {
            // The component providing this transport may have been dlopened
            // since the map was read. Refresh and ask again before refusing.
            refresh_ucx_code_ranges();
            ok = ops_layout_plausible(iface->ops, &bad);
        }
        if (!ok) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                std::fprintf(stderr,
                             "[pcomm] ucx_deep: REFUSING to instrument — uct_iface_ops_t does not "
                             "look like the struct this build compiled against: slot '%s' is not "
                             "executable UCX code. uct_send will be EMPTY. This is a layout "
                             "mismatch, not an absence of traffic.\n",
                             bad);
                std::fflush(stderr);
            }
        } else {
            std::lock_guard<std::mutex> lk(table_mu());
            Shadow*                     s = shadows().insert(iface);
            if (!s) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::fprintf(stderr,
                                 "[pcomm] ucx_deep: shadow table full (%zu interfaces); further "
                                 "interfaces will NOT be instrumented and their uct_send rows "
                                 "will be missing.\n",
                                 kMaxIfaces);
                    std::fflush(stderr);
                }
            } else {
                s->orig = iface->ops;  // BEFORE any slot is patched
                s->iface_h = iface_h;
                patched = patch(s, &iface->ops);
                instrumented =
                    patched ? rr::UctInstrumented::Yes : rr::UctInstrumented::NoDataSlots;
            }
        }
    }

    rec().record(span(begin, end), rr::UctIface{{},
                                                {},
                                                open_mode,
                                                iface_h,
                                                h(worker),
                                                h(md),
                                                tl_h,
                                                dev_h,
                                                static_cast<std::uint8_t>(instrumented),
                                                patched,
                                                0,
                                                0});
}

Profiler::Profiler() {
    const pcomm::Config cfg = pcomm::Config::from_env();
    if (pcomm_engine_declare_provider("ucx_deep", pcomm::core::id_range::ucx_deep_lo,
                                      pcomm::core::id_range::ucx_deep_hi,
                                      PCOMM_ENGINE_ABI_VERSION) != 0)
        return;
    pcomm_engine_acquire(cfg.output_dir.c_str(), cfg.rank, cfg.verbose ? 1 : 0);

    // One row, written once, saying what this process bound itself to. It is
    // emitted whether or not the guard accepted, because "uct_send is empty"
    // has two very different causes and a capture must distinguish them.
    unsigned maj = 0, min = 0, patch_v = 0;
    ucp_get_version(&maj, &min, &patch_v);
    rec().record(rec().now_instant(),
                 rr::UctBinding{{},
                                {},
                                pack_version(UCT_VERNO_MAJOR, UCT_VERNO_MINOR, UCT_VERNO_PATCH),
                                pack_version(maj, min, patch_v),
                                static_cast<std::uint32_t>(sizeof(uct_iface_ops_t)),
                                static_cast<std::uint8_t>(binding().accepted ? 1 : 0),
                                static_cast<std::uint8_t>(binding().forced ? 1 : 0),
                                0,
                                0});

    register_post<uctd::iface_open_tag>(
        [](ucs_status_t status, uct_md_h md, uct_worker_h worker, const uct_iface_params_t* params,
           const uct_iface_config_t*, uct_iface_h* iface_p, auto begin, auto end) {
            if (status != UCS_OK || !iface_p || !*iface_p)
                return;
            on_iface_open(md, worker, params, *iface_p, begin, end);
        });
}

template class UctEventSource<Profiler>;

}  // namespace ucx_deep

extern "C" ucs_status_t uct_iface_open(uct_md_h md, uct_worker_h worker,
                                       const uct_iface_params_t* params,
                                       const uct_iface_config_t* config, uct_iface_h* iface_p) {
    return ucx_deep::Profiler::invoke_preload<uctd::iface_open_tag>(md, worker, params, config,
                                                                    iface_p);
}

namespace pcomm::providers {
class UcxDeepProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"ucx_deep", core::id_range::ucx_deep_lo, core::id_range::ucx_deep_hi};
    }
    bool init(const provider::ProviderConfig&, rec::Recorder<>&) override {
        ::ucx_deep::Profiler::instance();
        return true;
    }
    void shutdown() override {
        pcomm_engine_drain_thread();
        pcomm_engine_release();
    }
};
}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::UcxDeepProvider)

static void __attribute__((constructor)) pcomm_ucx_deep_load() {
    ucx_deep::Profiler::instance();
}
static void __attribute__((destructor)) pcomm_ucx_deep_unload() {
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
