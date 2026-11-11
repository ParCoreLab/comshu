#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <ucp/api/ucp.h>
#include <ucs/type/status.h>

#include "comshu/core/mem_attr.hpp"
#include "comshu/core/records.hpp"
#include "comshu/engine_abi.h"
#include "comshu/provider/provider.hpp"
#include "comshu/rec/intern_table.hpp"
#include "comshu/tracer.hpp"

#include "ucx_event_source.hpp"
#include "ucx_records.hpp"

namespace ucx {
namespace rr = comshu::providers::ucx::records;
using comshu::rec::InternDomain;

static comshu::rec::Recorder<>& rec() {
    static comshu::rec::Recorder<> proxy;
    return proxy;
}

static comshu::core::Handle h(const void* p) {
    return rec().intern(InternDomain::Pointer, reinterpret_cast<std::uint64_t>(p));
}

static comshu::core::Temporal span(std::uint64_t begin, std::uint64_t end) {
    return comshu::core::Temporal::interval({comshu::core::kDomainHostTsc, begin},
                                           {comshu::core::kDomainHostTsc, end});
}

static void close(const void* p, comshu::core::RecordTypeId opener) {
    if (p)
        rec().record(rec().now_instant(), comshu::core::records::EntityClose{{}, {}, h(p), opener});
}

// ONE DESTRUCTION, ONE CLOSE — for endpoints, which are the only entity here
// with more than one destruction door.
//
// UCX reaches its own public close symbols through one another, and all four are
// interposed, so a single teardown fires several of these hooks. Measured on
// job 44698048: endpoint 4 on rank 0 closed THREE times at ticks ...560853,
// ...562743, ...563571 — 1.9 us and 0.8 us apart, which is nesting inside one
// destruction and not three application-level closes. Endpoints 7 the same, 20
// and 24 twice each.
//
// It went unseen because entity_close_accounted's `opens` CTE had no UCX tables,
// so every UCX close already reported as "names an entity no open record
// created" and the closes-outnumber-opens clause never got a denominator to
// compare against. Two defects, the louder one masking the quieter.
//
// AN UNKNOWN HANDLE STILL EMITS, and that is the important half. Suppressing on
// "not currently open" would silently swallow the close of an endpoint created
// through a door we do not hook — which is the single signal
// entity_close_accounted exists to deliver, and its comment says outright not to
// filter it away. So only a handle we have already closed is suppressed: state
// goes absent -> emit (unknown provenance, let the assertion flag it), open ->
// emit, closed -> drop. A create on the same address resets it to open, so
// pointer reuse after a genuine close still records both closes.
// true = open, false = closed. Absent = never seen, which is NOT the same as
// closed and is why this is a map rather than a set.
//
// LEAKED ON PURPOSE, and this is not a style choice. An endpoint can be
// destroyed during process teardown — this file registers its own
// __attribute__((destructor)), and an application or an MPI stack may close
// endpoints from an atexit handler, which runs AFTER the destructors of
// namespace-scope objects in this translation unit. A `std::unordered_map`
// there is a use-after-destruction: exactly the bug that made every gpumem case
// segfault at address 0x8 when a POD static became an unordered_map inside a
// path reachable from nvbit_at_term.
//
// gpumem's answer was a static_assert on trivial destructibility, which cannot
// apply to a map. The equivalent guarantee here is to never run the destructor
// at all: the pointer is trivially destructible, the objects it points at
// outlive every handler, and the memory is reclaimed by the OS at exit like any
// other allocation in a dying process. The mutex is immortal for the same
// reason — locking a destroyed mutex is no better than reading a destroyed map.
static std::mutex& ep_mu() {
    static std::mutex* m = new std::mutex();
    return *m;
}
static std::unordered_map<const void*, bool>& ep_open() {
    static auto* m = new std::unordered_map<const void*, bool>();
    return *m;
}

static void note_endpoint_open(const void* p) {
    if (!p)
        return;
    std::lock_guard<std::mutex> lk(ep_mu());
    ep_open()[p] = true;
}

static void close_endpoint_once(const void* p) {
    if (!p)
        return;
    {
        std::lock_guard<std::mutex> lk(ep_mu());
        auto&                       open = ep_open();
        auto                        it = open.find(p);
        if (it != open.end() && !it->second)
            return;  // already closed by an outer door in this same teardown
        open[p] = false;
    }
    close(p, comshu::core::RecordTrait<rr::UcxEndpoint>::type_id);
}

// FNV-1a, the same construction tools/schema_check.py uses for the schema hash,
// and chosen for the same property: it is deterministic everywhere. See the
// ucp_worker_get_address hook for why a process-salted hash would break the one
// join that crosses ranks.
static std::uint64_t fnv1a(const void* data, std::size_t len) {
    const auto*   bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Which declared parameter of ucp_ep_rkey_unpack_internal actually holds
// `rkey_p` on the libucp this process loaded. See the hook's comment in
// ucx_hooks.hpp for the two signatures and why the choice cannot be made at
// build time.
enum class RkeyOutParam { Unverified, Sixth, Seventh };

// The boundary, read off the release tags rather than inferred from the one
// version that happened to be in front of us:
//   v1.18.0  ... skip_md_map,             rkey_p)   -> 6th
//   v1.19.0  ... skip_md_map, sys_device, rkey_p)   -> 7th
// and upstream master still matches 1.19. UCX 1.x has never renumbered, so
// "1.19 or newer" is the test; a hypothetical 2.x is Unverified until someone
// reads its header, which is the safe direction to be wrong in.
static constexpr unsigned kSysDeviceMajor = 1;
static constexpr unsigned kSysDeviceMinor = 19;
static constexpr unsigned kOldestVerifiedMinor = 18;

// Asked once, of the LOADED library. ucp_get_version is public API and has been
// since 1.0, so this needs no hook and no dlsym dance of its own.
//
// The default is Unverified, not Sixth. Guessing an out-parameter position and
// being wrong does not crash and does not fail to build: it interns whatever
// qword sits in that register and writes it to ucx_rkey as a handle. A table of
// fabricated handles that join to nothing is strictly worse than an empty one,
// because the empty one is visibly a hole and the fabricated one is not.
// Nonzero on this thread while the PUBLIC ucp_ep_rkey_unpack runs. From UCX
// 1.19 the public door calls ucp_ep_rkey_unpack_internal through the PLT, so
// both hooks fired for one key and ucx_rkey held it twice (ucx_records on
// university-base, UCX 1.19.0: two rows, same rkey and endpoint, 8 us apart).
// MN5's 1.18 build does not nest, which is why no board saw it. The outer
// door records; the inner one stays quiet inside it, so either routing gives
// exactly one row — the shape reg_mr_depth() has in the verbs provider.
static int& rkey_unpack_depth() noexcept {
    static thread_local int depth = 0;
    return depth;
}

static RkeyOutParam rkey_out_param() noexcept {
    static const RkeyOutParam kLayout = [] {
        unsigned major = 0, minor = 0, release = 0;
        ucp_get_version(&major, &minor, &release);
        if (major != kSysDeviceMajor || minor < kOldestVerifiedMinor)
            return RkeyOutParam::Unverified;
        return minor >= kSysDeviceMinor ? RkeyOutParam::Seventh : RkeyOutParam::Sixth;
    }();
    return kLayout;
}

// How much of a UCX address the cross-rank join can see. Fixed rather than
// tunable because both sides must agree on it, and they are different processes
// that never negotiate: a capture written with a different prefix would produce
// hashes that silently never match.
static constexpr std::size_t kAddressHashPrefix = 32;

// Progress is Full-mode telemetry, and the mode is read ONCE for the reason the
// ibverbs provider reads its own once: this is the hottest door in the process
// (Stage 0 measured up to 617,094 calls per rank against 133 sends), and a
// getenv per call would be its own performance story.
static bool record_progress() noexcept {
    static const bool on =
        comshu_engine_trace_mode() >= static_cast<int>(comshu::core::TraceMode::Full);
    return on;
}

struct ClassifiedStatus {
    comshu::core::Handle request{};
    std::int32_t        status{UCS_OK};
    rr::StatusClass     kind{rr::StatusClass::Immediate};
};

static ClassifiedStatus classify(ucs_status_ptr_t value) {
    if (value == nullptr)
        return {};
    if (UCS_PTR_IS_ERR(value))
        return {comshu::core::kNullHandle, static_cast<std::int32_t>(UCS_PTR_STATUS(value)),
                rr::StatusClass::Error};
    return {h(value), UCS_INPROGRESS, rr::StatusClass::Request};
}

// ONE emit path per operation, shared by every door that reaches it. The legacy
// and modern entry points differ only in their argument lists and in the UcxDoor
// they stamp, so giving each its own record-building code would be four more
// places for the field order to drift silently. See UcxDoor.
static void emit_tag_send(ucp_ep_h endpoint, const void* buffer, size_t count, ucp_tag_t tag,
                          ucs_status_ptr_t result, rr::UcxDoor door, std::uint64_t begin,
                          std::uint64_t end) {
    const auto s = classify(result);
    rec().record(span(begin, end), rr::UcxTagSend{{},
                                                  {},
                                                  h(endpoint),
                                                  h(buffer),
                                                  s.request,
                                                  count,
                                                  tag,
                                                  s.status,
                                                  static_cast<std::uint8_t>(s.kind),
                                                  static_cast<std::uint8_t>(door),
                                                  comshu::core::device_of(buffer)});
}

static void emit_tag_recv(ucp_worker_h worker, void* buffer, size_t count, ucp_tag_t tag,
                          ucp_tag_t tag_mask, ucs_status_ptr_t result, rr::UcxDoor door,
                          std::uint64_t begin, std::uint64_t end, const void* message = nullptr) {
    const auto s = classify(result);
    rec().record(span(begin, end), rr::UcxTagRecv{{},
                                                  {},
                                                  h(worker),
                                                  h(buffer),
                                                  s.request,
                                                  count,
                                                  tag,
                                                  tag_mask,
                                                  message ? h(message) : comshu::core::kNullHandle,
                                                  s.status,
                                                  static_cast<std::uint8_t>(s.kind),
                                                  static_cast<std::uint8_t>(door)});
}

// The three atomic opcode spaces, mapped to one. See UcxAtomicOp: the raw values
// collide across doors, so this mapping is the only thing that makes the field
// readable, and it is done here rather than in the reader because only the hook
// knows which door it came from.
static rr::UcxAtomicOp map_post_op(ucp_atomic_post_op_t op) {
    switch (op) {
        case UCP_ATOMIC_POST_OP_ADD:
            return rr::UcxAtomicOp::Add;
        case UCP_ATOMIC_POST_OP_AND:
            return rr::UcxAtomicOp::And;
        case UCP_ATOMIC_POST_OP_OR:
            return rr::UcxAtomicOp::Or;
        case UCP_ATOMIC_POST_OP_XOR:
            return rr::UcxAtomicOp::Xor;
        default:
            return rr::UcxAtomicOp::Unknown;
    }
}
static rr::UcxAtomicOp map_fetch_op(ucp_atomic_fetch_op_t op) {
    switch (op) {
        case UCP_ATOMIC_FETCH_OP_FADD:
            return rr::UcxAtomicOp::Add;
        case UCP_ATOMIC_FETCH_OP_SWAP:
            return rr::UcxAtomicOp::Swap;
        case UCP_ATOMIC_FETCH_OP_CSWAP:
            return rr::UcxAtomicOp::CompareSwap;
        case UCP_ATOMIC_FETCH_OP_FAND:
            return rr::UcxAtomicOp::And;
        case UCP_ATOMIC_FETCH_OP_FOR:
            return rr::UcxAtomicOp::Or;
        case UCP_ATOMIC_FETCH_OP_FXOR:
            return rr::UcxAtomicOp::Xor;
        default:
            return rr::UcxAtomicOp::Unknown;
    }
}
static rr::UcxAtomicOp map_nbx_op(ucp_atomic_op_t op) {
    switch (op) {
        case UCP_ATOMIC_OP_ADD:
            return rr::UcxAtomicOp::Add;
        case UCP_ATOMIC_OP_SWAP:
            return rr::UcxAtomicOp::Swap;
        case UCP_ATOMIC_OP_CSWAP:
            return rr::UcxAtomicOp::CompareSwap;
        case UCP_ATOMIC_OP_AND:
            return rr::UcxAtomicOp::And;
        case UCP_ATOMIC_OP_OR:
            return rr::UcxAtomicOp::Or;
        case UCP_ATOMIC_OP_XOR:
            return rr::UcxAtomicOp::Xor;
        default:
            return rr::UcxAtomicOp::Unknown;
    }
}

static void emit_atomic(ucp_ep_h endpoint, const void* buffer, ClassifiedStatus s,
                        std::uint64_t value, std::uint64_t remote_addr, ucp_rkey_h rkey,
                        std::uint64_t op_size, rr::UcxAtomicOp op, rr::UcxDoor door, bool fetching,
                        std::uint64_t begin, std::uint64_t end) {
    rec().record(span(begin, end), rr::UcxAtomic{{},
                                                 {},
                                                 h(endpoint),
                                                 buffer ? h(buffer) : comshu::core::kNullHandle,
                                                 s.request,
                                                 h(rkey),
                                                 value,
                                                 remote_addr,
                                                 op_size,
                                                 s.status,
                                                 static_cast<std::uint8_t>(s.kind),
                                                 static_cast<std::uint8_t>(op),
                                                 static_cast<std::uint8_t>(door),
                                                 static_cast<std::uint8_t>(fetching ? 1 : 0)});
}

static void emit_rma(ucp_ep_h endpoint, const void* buffer, size_t count, std::uint64_t remote_addr,
                     ucp_rkey_h rkey, ClassifiedStatus s, rr::RmaOp op, rr::UcxDoor door,
                     std::uint64_t begin, std::uint64_t end) {
    rec().record(span(begin, end), rr::UcxRma{{},
                                              {},
                                              h(endpoint),
                                              h(buffer),
                                              s.request,
                                              h(rkey),
                                              count,
                                              remote_addr,
                                              s.status,
                                              static_cast<std::uint8_t>(s.kind),
                                              static_cast<std::uint8_t>(op),
                                              static_cast<std::uint8_t>(door),
                                              comshu::core::device_of(buffer)});
}

// A `_nbi` call returns a plain status and never a request: the operation is
// issued and there is nothing left to wait on, so it classifies as Immediate on
// success and Error otherwise, and never as Request.
static ClassifiedStatus classify_status(ucs_status_t status) {
    if (status == UCS_OK)
        return {};
    return {comshu::core::kNullHandle, static_cast<std::int32_t>(status), rr::StatusClass::Error};
}

// UCX IMPLEMENTS ITS LEGACY DOORS ON TOP OF THE MODERN ONES, and because our
// interposers are global that internal call lands on ours too. Measured on the
// self transport before this guard existed: a program issuing three tagged sends
// (one _nbx, two _nb) produced FIVE ucx_tag_send rows — each legacy send once as
// `nb` and again as `nbx`, same tag, same length, microseconds apart. Receives
// did the same.
//
// This is the ibv_reg_mr / ibv_reg_mr_iova2 defect exactly (see ib_hooks.hpp),
// down to the remedy: a thread-local depth counter, with the OUTER door as the
// recorder and the nested inner call suppressed. The outer one is also the
// truthful one — it is the door the application actually called, which is the
// whole point of UcxDoor.
//
// A counter rather than a flag because nothing promises the nesting is only one
// deep, and it is per-thread because the nesting is per-call-stack. Whether a
// given UCX build routes a given legacy door through its modern twin is a fact
// observable at run time; which UCX build you are on is not.
static int& legacy_depth() noexcept {
    static thread_local int depth = 0;
    return depth;
}

// True when this call is UCX's own, made from inside a legacy door we are
// already recording.
static bool nested_in_legacy() noexcept {
    return legacy_depth() > 0;
}

Profiler::Profiler() {
    const comshu::Config cfg = comshu::Config::from_env();
    if (comshu_engine_declare_provider("ucx", comshu::core::id_range::ucx_lo,
                                      comshu::core::id_range::ucx_hi, COMSHU_ENGINE_ABI_VERSION) != 0)
        return;
    comshu_engine_acquire(cfg.output_dir.c_str(), cfg.rank, cfg.verbose ? 1 : 0);

    register_post<init_version_tag>([](ucs_status_t status, unsigned major, unsigned minor,
                                       const ucp_params_t*, const ucp_config_t*, ucp_context_h* out,
                                       auto begin, auto end) {
        if (status == UCS_OK && out && *out)
            rec().record(span(begin, end), rr::UcxContext{{}, {}, h(*out), major, minor});
    });
    register_pre<cleanup_tag>([](ucp_context_h context, auto) {
        close(context, comshu::core::RecordTrait<rr::UcxContext>::type_id);
    });
    register_post<worker_create_tag>([](ucs_status_t status, ucp_context_h        context,
                                        const ucp_worker_params_t*, ucp_worker_h* out, auto begin,
                                        auto end) {
        if (status == UCS_OK && out && *out)
            rec().record(span(begin, end), rr::UcxWorker{{}, {}, h(*out), h(context)});
    });
    register_post<worker_get_address_tag>([](ucs_status_t status, ucp_worker_h worker,
                                             ucp_address_t** address, size_t* length, auto begin,
                                             auto end) {
        if (status != UCS_OK || !address || !*address || !length || *length < kAddressHashPrefix)
            return;
        rec().record(span(begin, end), rr::UcxWorkerAddress{{},
                                                            {},
                                                            h(worker),
                                                            fnv1a(*address, *length),
                                                            fnv1a(*address, kAddressHashPrefix),
                                                            static_cast<std::uint32_t>(*length)});
    });
    // The same record, from the door OpenMPI's point-to-point path actually
    // uses. See the ucp_worker_query hook in ucx_hooks.hpp for why this exists
    // and what was silently empty without it.
    //
    // THE FIELD MASK IS LOAD-BEARING, and is the whole difference between this
    // handler and the one above. `ucp_worker_get_address` has one job and its
    // out-parameters are always meaningful on UCS_OK. `ucp_worker_query` fills
    // only what the CALLER asked for: `attr->address` and `attr->address_length`
    // are untouched — not zeroed, untouched — unless UCP_WORKER_ATTR_FIELD_ADDRESS
    // was set on input. Reading them regardless would hash whatever the caller's
    // stack happened to hold and mint a plausible, deterministic, wrong join key,
    // which is worse than no row: `remote_prefix_hash` would match nothing while
    // looking perfectly well-formed. Most callers of this function want
    // thread_mode or max_am_header and never ask for the address at all.
    register_post<worker_query_tag>(
        [](ucs_status_t status, ucp_worker_h worker, ucp_worker_attr_t* attr, auto begin, auto end) {
            if (status != UCS_OK || !attr)
                return;
            if (!(attr->field_mask & UCP_WORKER_ATTR_FIELD_ADDRESS))
                return;
            if (!attr->address || attr->address_length < kAddressHashPrefix)
                return;
            rec().record(span(begin, end),
                         rr::UcxWorkerAddress{
                             {},
                             {},
                             h(worker),
                             fnv1a(attr->address, attr->address_length),
                             fnv1a(attr->address, kAddressHashPrefix),
                             static_cast<std::uint32_t>(attr->address_length)});
        });
    register_pre<worker_destroy_tag>([](ucp_worker_h worker, auto) {
        close(worker, comshu::core::RecordTrait<rr::UcxWorker>::type_id);
    });
    register_post<ep_create_tag>([](ucs_status_t status, ucp_worker_h worker,
                                    const ucp_ep_params_t* params, ucp_ep_h* out, auto begin,
                                    auto end) {
        if (status != UCS_OK || !out || !*out)
            return;
        rec().record(span(begin, end), rr::UcxEndpoint{{}, {}, h(*out), h(worker)});
        note_endpoint_open(*out);
        // Only when the caller really passed a peer address. An endpoint created
        // from a socket address or a connection request has none, and a row of
        // zeroes there would read like a measured null peer.
        if (params && (params->field_mask & UCP_EP_PARAM_FIELD_REMOTE_ADDRESS) && params->address)
            rec().record(
                span(begin, end),
                rr::UcxEpConnection{{}, {}, h(*out), fnv1a(params->address, kAddressHashPrefix)});
    });
    register_pre<ep_close_nbx_tag>(
        [](ucp_ep_h endpoint, const ucp_request_param_t*, auto) { close_endpoint_once(endpoint); });
    register_post<tag_send_nbx_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint,
                                       const void* buffer, size_t count, ucp_tag_t tag,
                                       const ucp_request_param_t*, auto begin, auto end) {
        if (nested_in_legacy())
            return;
        emit_tag_send(endpoint, buffer, count, tag, result, rr::UcxDoor::Nbx, begin, end);
    });
    // A receive names its WORKER, not an endpoint: the match is by tag across
    // every peer, so there is no endpoint to record. See UcxTagRecv.
    register_post<tag_recv_nbx_tag>([](ucs_status_ptr_t result, ucp_worker_h worker, void* buffer,
                                       size_t count, ucp_tag_t tag, ucp_tag_t tag_mask,
                                       const ucp_request_param_t*, auto begin, auto end) {
        if (nested_in_legacy())
            return;
        emit_tag_recv(worker, buffer, count, tag, tag_mask, result, rr::UcxDoor::Nbx, begin, end);
    });
    register_post<put_nbx_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint, const void* buffer,
                                  size_t count, std::uint64_t remote_addr, ucp_rkey_h rkey,
                                  const ucp_request_param_t*, auto begin, auto end) {
        if (nested_in_legacy())
            return;
        emit_rma(endpoint, buffer, count, remote_addr, rkey, classify(result), rr::RmaOp::Put,
                 rr::UcxDoor::Nbx, begin, end);
    });
    register_post<get_nbx_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint, void* buffer,
                                  size_t count, std::uint64_t remote_addr, ucp_rkey_h rkey,
                                  const ucp_request_param_t*, auto begin, auto end) {
        if (nested_in_legacy())
            return;
        emit_rma(endpoint, buffer, count, remote_addr, rkey, classify(result), rr::RmaOp::Get,
                 rr::UcxDoor::Nbx, begin, end);
    });
    register_post<am_send_nbx_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint, unsigned am_id,
                                      const void*, size_t header_length, const void* buffer,
                                      size_t count, const ucp_request_param_t*, auto begin,
                                      auto end) {
        const auto s = classify(result);
        rec().record(span(begin, end), rr::UcxAmSend{{},
                                                     {},
                                                     h(endpoint),
                                                     h(buffer),
                                                     s.request,
                                                     am_id,
                                                     count,
                                                     header_length,
                                                     s.status,
                                                     static_cast<std::uint8_t>(s.kind),
                                                     comshu::core::device_of(buffer)});
    });
    // EVERY optional field is read through its field_mask bit, never assumed
    // present: ucp_mem_map_params_t is a masked struct, and a caller passing
    // UCP_MEM_MAP_ALLOCATE supplies no address at all. Reading one anyway would
    // put whatever the caller left on the stack into the capture as an address.
    //
    // The effective values are then preferred over the requested ones, because
    // they are what UCX actually registered — the two differ on every page
    // rounding and on every allocate-for-me call, where the requested address is
    // not merely imprecise but absent.
    register_post<mem_map_tag>([](ucs_status_t status, ucp_context_h context,
                                  const ucp_mem_map_params_t* params, ucp_mem_h* out, auto begin,
                                  auto end) {
        if (status != UCS_OK || !out || !*out)
            return;
        std::uint64_t address = 0;
        std::uint64_t length = 0;
        if (params) {
            if (params->field_mask & UCP_MEM_MAP_PARAM_FIELD_ADDRESS)
                address = reinterpret_cast<std::uint64_t>(params->address);
            if (params->field_mask & UCP_MEM_MAP_PARAM_FIELD_LENGTH)
                length = params->length;
        }
        std::uint8_t   resolved = 0;
        ucp_mem_attr_t attr{};
        attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS | UCP_MEM_ATTR_FIELD_LENGTH;
        if (ucp_mem_query(*out, &attr) == UCS_OK) {
            address = reinterpret_cast<std::uint64_t>(attr.address);
            length = attr.length;
            resolved = 1;
        }
        rec().record(span(begin, end),
                     rr::UcxMemMap{{}, {}, h(*out), h(context), address, length, resolved});
    });
    register_pre<mem_unmap_tag>([](ucp_context_h, ucp_mem_h memh, auto) {
        close(memh, comshu::core::RecordTrait<rr::UcxMemMap>::type_id);
    });
    // The remote keys. See UcxRkey for why this record exists and for the one
    // thing it deliberately does not carry.
    register_post<ep_rkey_unpack_tag>([](ucs_status_t status, ucp_ep_h endpoint, const void*,
                                         ucp_rkey_h* out, auto begin, auto end) {
        if (status != UCS_OK || !out || !*out)
            return;
        rec().record(span(begin, end), rr::UcxRkey{{}, {}, h(*out), h(endpoint)});
    });
    register_pre<rkey_destroy_tag>(
        [](ucp_rkey_h rkey, auto) { close(rkey, comshu::core::RecordTrait<rr::UcxRkey>::type_id); });
    // The rendezvous door. ucx_hooks.hpp explains why this symbol is declared
    // with one more parameter than it has here; this is where that is resolved.
    //
    // `a6` is rkey_p up to and including 1.18, `a7` from 1.19 on. Asking the
    // LOADED library rather than a header, because this .so is dlopened against
    // whichever libucp the application brought with it.
    //
    // Outside the range the two signatures were actually read from, record
    // NOTHING and still forward. A version older than 1.18 has not been checked
    // and could put rkey_p somewhere else entirely — and interning the wrong
    // qword would not fail, it would fill ucx_rkey with fabricated handles that
    // join to nothing. An unrecorded key leaves case 21 red and honest; a
    // fabricated one makes it green and wrong.
    register_post<ep_rkey_unpack_internal_tag>([](ucs_status_t status, ucp_ep_h endpoint,
                                                  const void*, std::size_t, std::uint64_t,
                                                  std::uint64_t, std::uint64_t a6, std::uint64_t a7,
                                                  auto begin, auto end) {
        if (status != UCS_OK || rkey_unpack_depth() > 0)
            return;
        const auto layout = rkey_out_param();
        if (layout == RkeyOutParam::Unverified)
            return;
        auto* out = reinterpret_cast<ucp_rkey_h*>(layout == RkeyOutParam::Seventh ? a7 : a6);
        if (!out || !*out)
            return;
        rec().record(span(begin, end), rr::UcxRkey{{}, {}, h(*out), h(endpoint)});
    });
    // THE SIZE IS READ BEFORE IT IS TRUSTED. `ucp_rkey_pack` sets both
    // out-parameters only on UCS_OK, and a dummy key is a legitimate 9-byte
    // buffer, so the hash covers exactly `*size` bytes and never a fixed prefix
    // — the whole reason the consuming side gets no hash at all.
    register_post<rkey_pack_tag>([](ucs_status_t status, ucp_context_h context, ucp_mem_h memh,
                                    void** buffer, size_t* size, auto begin, auto end) {
        if (status != UCS_OK || !buffer || !*buffer || !size || *size == 0)
            return;
        rec().record(span(begin, end), rr::UcxRkeyPack{{},
                                                       {},
                                                       h(memh),
                                                       h(context),
                                                       fnv1a(*buffer, *size),
                                                       static_cast<std::uint32_t>(*size)});
    });
    // Empty polls are already dropped by the hook's predicate; this drops the
    // rest outside Full mode. Both filters are needed — the predicate cuts the
    // firehose, the mode decides whether the survivors are wanted at all.
    register_post<worker_progress_tag>(
        [](unsigned progressed, ucp_worker_h worker, auto begin, auto end) {
            if (!record_progress())
                return;
            rec().record(span(begin, end), rr::UcxWorkerProgress{{}, {}, h(worker), progressed});
        });

    // --- the doors MN5's stack actually calls; see ucx_hooks.hpp -------------
    // Three more endpoint close doors. Without them ucx_endpoint leaks: hcoll
    // and the NCCL plugin close through ucp_ep_close_nb, OpenSHMEM and MPI
    // one-sided through ucp_ep_destroy, libmca_common_ucx through
    // ucp_disconnect_nb. All four closes are the SAME entity, so they emit the
    // same EntityClose against UcxEndpoint rather than each minting its own.
    register_pre<ep_close_nb_tag>(
        [](ucp_ep_h endpoint, unsigned, auto) { close_endpoint_once(endpoint); });
    register_pre<ep_destroy_tag>([](ucp_ep_h endpoint, auto) { close_endpoint_once(endpoint); });
    register_pre<disconnect_nb_tag>([](ucp_ep_h endpoint, auto) { close_endpoint_once(endpoint); });

    // The one-sided doors MPI actually uses. mca_osc_ucx references only these,
    // never the _nbx pair, so without them MPI_Put/MPI_Get produce an empty
    // ucx_rma table — a confident zero rather than a gap.
    register_post<put_nbi_tag>([](ucs_status_t status, ucp_ep_h endpoint, const void* buffer,
                                  size_t length, std::uint64_t remote_addr, ucp_rkey_h rkey,
                                  auto begin, auto end) {
        --legacy_depth();
        emit_rma(endpoint, buffer, length, remote_addr, rkey, classify_status(status),
                 rr::RmaOp::Put, rr::UcxDoor::Nbi, begin, end);
    });
    register_post<get_nbi_tag>([](ucs_status_t status, ucp_ep_h endpoint, void* buffer,
                                  size_t length, std::uint64_t remote_addr, ucp_rkey_h rkey,
                                  auto begin, auto end) {
        --legacy_depth();
        emit_rma(endpoint, buffer, length, remote_addr, rkey, classify_status(status),
                 rr::RmaOp::Get, rr::UcxDoor::Nbi, begin, end);
    });
    register_post<get_nb_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint, void* buffer,
                                 size_t length, std::uint64_t remote_addr, ucp_rkey_h rkey,
                                 ucp_send_callback_t, auto begin, auto end) {
        --legacy_depth();
        emit_rma(endpoint, buffer, length, remote_addr, rkey, classify(result), rr::RmaOp::Get,
                 rr::UcxDoor::Legacy, begin, end);
    });

    // Every legacy door opens the nesting scope on ENTRY and closes it on exit,
    // so the modern door it is implemented over recognises its own call and
    // stays quiet. See legacy_depth().
    register_pre<tag_send_nb_tag>([](ucp_ep_h, const void*, size_t, ucp_datatype_t, ucp_tag_t,
                                     ucp_send_callback_t, auto) { ++legacy_depth(); });
    register_pre<tag_send_sync_nb_tag>([](ucp_ep_h, const void*, size_t, ucp_datatype_t, ucp_tag_t,
                                          ucp_send_callback_t, auto) { ++legacy_depth(); });
    register_pre<tag_recv_nb_tag>([](ucp_worker_h, void*, size_t, ucp_datatype_t, ucp_tag_t,
                                     ucp_tag_t, ucp_tag_recv_callback_t,
                                     auto) { ++legacy_depth(); });
    register_pre<get_nb_tag>([](ucp_ep_h, void*, size_t, std::uint64_t, ucp_rkey_h,
                                ucp_send_callback_t, auto) { ++legacy_depth(); });
    register_pre<put_nbi_tag>(
        [](ucp_ep_h, const void*, size_t, std::uint64_t, ucp_rkey_h, auto) { ++legacy_depth(); });
    register_pre<get_nbi_tag>(
        [](ucp_ep_h, void*, size_t, std::uint64_t, ucp_rkey_h, auto) { ++legacy_depth(); });
    register_pre<atomic_post_tag>([](ucp_ep_h, ucp_atomic_post_op_t, std::uint64_t, size_t,
                                     std::uint64_t, ucp_rkey_h, auto) { ++legacy_depth(); });
    register_pre<atomic_fetch_nb_tag>([](ucp_ep_h, ucp_atomic_fetch_op_t, std::uint64_t, void*,
                                         size_t, std::uint64_t, ucp_rkey_h, ucp_send_callback_t,
                                         auto) { ++legacy_depth(); });
    register_pre<tag_msg_recv_nb_tag>([](ucp_worker_h, void*, size_t, ucp_datatype_t,
                                         ucp_tag_message_h, ucp_tag_recv_callback_t,
                                         auto) { ++legacy_depth(); });

    // The legacy tagged doors. mca_pml_ucx, hcoll and SHARP all reference them,
    // and ucp_tag_send_sync_nb is kept distinct because it is MPI_Ssend: it
    // completes only once the receiver has matched.
    register_post<tag_send_nb_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint,
                                      const void* buffer, size_t count, ucp_datatype_t,
                                      ucp_tag_t tag, ucp_send_callback_t, auto begin, auto end) {
        --legacy_depth();
        emit_tag_send(endpoint, buffer, count, tag, result, rr::UcxDoor::Legacy, begin, end);
    });
    register_post<tag_send_sync_nb_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint,
                                           const void* buffer, size_t count, ucp_datatype_t,
                                           ucp_tag_t tag, ucp_send_callback_t, auto begin,
                                           auto end) {
        --legacy_depth();
        emit_tag_send(endpoint, buffer, count, tag, result, rr::UcxDoor::LegacySync, begin, end);
    });
    register_post<tag_recv_nb_tag>(
        [](ucs_status_ptr_t result, ucp_worker_h worker, void* buffer, size_t count, ucp_datatype_t,
           ucp_tag_t tag, ucp_tag_t tag_mask, ucp_tag_recv_callback_t, auto begin, auto end) {
            --legacy_depth();
            emit_tag_recv(worker, buffer, count, tag, tag_mask, result, rr::UcxDoor::Legacy, begin,
                          end);
        });

    // The probe is the TAG SOURCE for ucp_tag_msg_recv_nb, which carries none.
    // Empty probes never reach here — the hook predicate drops them.
    register_post<tag_probe_nb_tag>([](ucp_tag_message_h found, ucp_worker_h worker, ucp_tag_t tag,
                                       ucp_tag_t tag_mask, int remove, ucp_tag_recv_info_t* info,
                                       auto begin, auto end) {
        rec().record(span(begin, end), rr::UcxTagProbe{{},
                                                       {},
                                                       h(worker),
                                                       h(found),
                                                       info ? info->sender_tag : 0,
                                                       info ? info->length : 0,
                                                       tag,
                                                       tag_mask,
                                                       static_cast<std::uint8_t>(remove ? 1 : 0)});
    });
    // tag and tag_mask are 0 here and that is not a gap being papered over: this
    // door receives a message ALREADY matched, so it genuinely has neither. The
    // message handle is the join key back to the probe that does. See UcxTagRecv.
    register_post<tag_msg_recv_nb_tag>(
        [](ucs_status_ptr_t result, ucp_worker_h worker, void* buffer, size_t count, ucp_datatype_t,
           ucp_tag_message_h message, ucp_tag_recv_callback_t, auto begin, auto end) {
            --legacy_depth();
            emit_tag_recv(worker, buffer, count, 0, 0, result, rr::UcxDoor::LegacyMsg, begin, end,
                          message);
        });

    // The atomics. ucp_atomic_post never returns a result, so it is never
    // fetching; ucp_atomic_fetch_nb always is; ucp_atomic_op_nbx is whichever
    // the caller's reply buffer says, read from the field mask rather than
    // guessed.
    register_post<atomic_post_tag>(
        [](ucs_status_t status, ucp_ep_h endpoint, ucp_atomic_post_op_t op, std::uint64_t value,
           size_t op_size, std::uint64_t remote_addr, ucp_rkey_h rkey, auto begin, auto end) {
            --legacy_depth();
            emit_atomic(endpoint, nullptr, classify_status(status), value, remote_addr, rkey,
                        op_size, map_post_op(op), rr::UcxDoor::LegacyPost, false, begin, end);
        });
    register_post<atomic_fetch_nb_tag>(
        [](ucs_status_ptr_t result, ucp_ep_h endpoint, ucp_atomic_fetch_op_t op,
           std::uint64_t value, void* fetch_result, size_t op_size, std::uint64_t remote_addr,
           ucp_rkey_h rkey, ucp_send_callback_t, auto begin, auto end) {
            --legacy_depth();
            emit_atomic(endpoint, fetch_result, classify(result), value, remote_addr, rkey, op_size,
                        map_fetch_op(op), rr::UcxDoor::Legacy, true, begin, end);
        });
    register_post<atomic_op_nbx_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint,
                                        ucp_atomic_op_t op, const void* buffer, size_t count,
                                        std::uint64_t remote_addr, ucp_rkey_h rkey,
                                        const ucp_request_param_t* param, auto begin, auto end) {
        const bool fetching = param && (param->op_attr_mask & UCP_OP_ATTR_FIELD_REPLY_BUFFER) != 0;
        if (nested_in_legacy())
            return;
        emit_atomic(endpoint, buffer, classify(result), 0, remote_addr, rkey, count, map_nbx_op(op),
                    rr::UcxDoor::Nbx, fetching, begin, end);
    });
}

template class UcxEventSource<Profiler>;
}  // namespace ucx

extern "C" ucs_status_t ucp_init_version(unsigned major, unsigned minor, const ucp_params_t* params,
                                         const ucp_config_t* config, ucp_context_h* context) {
    return ucx::Profiler::invoke_preload<ucx::init_version_tag>(major, minor, params, config,
                                                                context);
}
extern "C" void ucp_cleanup(ucp_context_h context) {
    ucx::Profiler::invoke_preload<ucx::cleanup_tag>(context);
}
extern "C" ucs_status_t ucp_worker_create(ucp_context_h context, const ucp_worker_params_t* params,
                                          ucp_worker_h* worker) {
    return ucx::Profiler::invoke_preload<ucx::worker_create_tag>(context, params, worker);
}
extern "C" void ucp_worker_destroy(ucp_worker_h worker) {
    ucx::Profiler::invoke_preload<ucx::worker_destroy_tag>(worker);
}
extern "C" ucs_status_t ucp_ep_create(ucp_worker_h worker, const ucp_ep_params_t* params,
                                      ucp_ep_h* endpoint) {
    return ucx::Profiler::invoke_preload<ucx::ep_create_tag>(worker, params, endpoint);
}
extern "C" ucs_status_ptr_t ucp_ep_close_nbx(ucp_ep_h endpoint, const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::ep_close_nbx_tag>(endpoint, params);
}
extern "C" ucs_status_ptr_t ucp_tag_send_nbx(ucp_ep_h endpoint, const void* buffer, size_t count,
                                             ucp_tag_t tag, const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::tag_send_nbx_tag>(endpoint, buffer, count, tag,
                                                                params);
}
extern "C" ucs_status_ptr_t ucp_tag_recv_nbx(ucp_worker_h worker, void* buffer, size_t count,
                                             ucp_tag_t tag, ucp_tag_t tag_mask,
                                             const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::tag_recv_nbx_tag>(worker, buffer, count, tag,
                                                                tag_mask, params);
}
extern "C" ucs_status_ptr_t ucp_put_nbx(ucp_ep_h endpoint, const void* buffer, size_t count,
                                        uint64_t remote_addr, ucp_rkey_h rkey,
                                        const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::put_nbx_tag>(endpoint, buffer, count, remote_addr,
                                                           rkey, params);
}
extern "C" ucs_status_ptr_t ucp_get_nbx(ucp_ep_h endpoint, void* buffer, size_t count,
                                        uint64_t remote_addr, ucp_rkey_h rkey,
                                        const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::get_nbx_tag>(endpoint, buffer, count, remote_addr,
                                                           rkey, params);
}
extern "C" ucs_status_ptr_t ucp_am_send_nbx(ucp_ep_h endpoint, unsigned am_id, const void* header,
                                            size_t header_length, const void* buffer, size_t count,
                                            const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::am_send_nbx_tag>(
        endpoint, am_id, header, header_length, buffer, count, params);
}
extern "C" ucs_status_t ucp_mem_map(ucp_context_h context, const ucp_mem_map_params_t* params,
                                    ucp_mem_h* memh) {
    return ucx::Profiler::invoke_preload<ucx::mem_map_tag>(context, params, memh);
}
extern "C" ucs_status_t ucp_mem_unmap(ucp_context_h context, ucp_mem_h memh) {
    return ucx::Profiler::invoke_preload<ucx::mem_unmap_tag>(context, memh);
}
extern "C" ucs_status_t ucp_worker_query(ucp_worker_h worker, ucp_worker_attr_t* attr) {
    return ucx::Profiler::invoke_preload<ucx::worker_query_tag>(worker, attr);
}
extern "C" ucs_status_t ucp_ep_rkey_unpack(ucp_ep_h endpoint, const void* rkey_buffer,
                                           ucp_rkey_h* rkey_p) {
    struct Depth {
        Depth() noexcept { ++ucx::rkey_unpack_depth(); }
        ~Depth() noexcept { --ucx::rkey_unpack_depth(); }
    } depth;
    return ucx::Profiler::invoke_preload<ucx::ep_rkey_unpack_tag>(endpoint, rkey_buffer, rkey_p);
}
extern "C" ucs_status_t ucp_rkey_pack(ucp_context_h context, ucp_mem_h memh, void** rkey_buffer_p,
                                      size_t* size_p) {
    return ucx::Profiler::invoke_preload<ucx::rkey_pack_tag>(context, memh, rkey_buffer_p, size_p);
}
extern "C" void ucp_rkey_destroy(ucp_rkey_h rkey) {
    ucx::Profiler::invoke_preload<ucx::rkey_destroy_tag>(rkey);
}
// Seven parameters for a function that takes six on MN5's UCX 1.18. Deliberate,
// and safe in both directions under the SysV AMD64 ABI: forwarding a seventh
// integer argument to a six-parameter callee is ignored by the callee, and the
// seventh slot is only ever *read* when rkey_out_param() says the loaded library
// really passes one. ucx_hooks.hpp carries the full argument.
//
// Named a6/a7 rather than sys_device/rkey_p because neither name is true on both
// versions, and a comment is a worse guard than a name that refuses to lie.
extern "C" ucs_status_t ucp_ep_rkey_unpack_internal(ucp_ep_h endpoint, const void* rkey_buffer,
                                                    std::size_t length, std::uint64_t unpack_md_map,
                                                    std::uint64_t skip_md_map, std::uint64_t a6,
                                                    std::uint64_t a7) {
    return ucx::Profiler::invoke_preload<ucx::ep_rkey_unpack_internal_tag>(
        endpoint, rkey_buffer, length, unpack_md_map, skip_md_map, a6, a7);
}
extern "C" ucs_status_t ucp_worker_get_address(ucp_worker_h worker, ucp_address_t** address,
                                               size_t* length) {
    return ucx::Profiler::invoke_preload<ucx::worker_get_address_tag>(worker, address, length);
}
extern "C" unsigned ucp_worker_progress(ucp_worker_h worker) {
    return ucx::Profiler::invoke_preload<ucx::worker_progress_tag>(worker);
}
extern "C" ucs_status_ptr_t ucp_ep_close_nb(ucp_ep_h endpoint, unsigned mode) {
    return ucx::Profiler::invoke_preload<ucx::ep_close_nb_tag>(endpoint, mode);
}
extern "C" void ucp_ep_destroy(ucp_ep_h endpoint) {
    ucx::Profiler::invoke_preload<ucx::ep_destroy_tag>(endpoint);
}
extern "C" ucs_status_ptr_t ucp_disconnect_nb(ucp_ep_h endpoint) {
    return ucx::Profiler::invoke_preload<ucx::disconnect_nb_tag>(endpoint);
}
extern "C" ucs_status_t ucp_put_nbi(ucp_ep_h endpoint, const void* buffer, size_t length,
                                    uint64_t remote_addr, ucp_rkey_h rkey) {
    return ucx::Profiler::invoke_preload<ucx::put_nbi_tag>(endpoint, buffer, length, remote_addr,
                                                           rkey);
}
extern "C" ucs_status_t ucp_get_nbi(ucp_ep_h endpoint, void* buffer, size_t length,
                                    uint64_t remote_addr, ucp_rkey_h rkey) {
    return ucx::Profiler::invoke_preload<ucx::get_nbi_tag>(endpoint, buffer, length, remote_addr,
                                                           rkey);
}
extern "C" ucs_status_ptr_t ucp_get_nb(ucp_ep_h endpoint, void* buffer, size_t length,
                                       uint64_t remote_addr, ucp_rkey_h rkey,
                                       ucp_send_callback_t cb) {
    return ucx::Profiler::invoke_preload<ucx::get_nb_tag>(endpoint, buffer, length, remote_addr,
                                                          rkey, cb);
}
extern "C" ucs_status_ptr_t ucp_tag_send_nb(ucp_ep_h endpoint, const void* buffer, size_t count,
                                            ucp_datatype_t datatype, ucp_tag_t tag,
                                            ucp_send_callback_t cb) {
    return ucx::Profiler::invoke_preload<ucx::tag_send_nb_tag>(endpoint, buffer, count, datatype,
                                                               tag, cb);
}
extern "C" ucs_status_ptr_t ucp_tag_send_sync_nb(ucp_ep_h endpoint, const void* buffer,
                                                 size_t count, ucp_datatype_t datatype,
                                                 ucp_tag_t tag, ucp_send_callback_t cb) {
    return ucx::Profiler::invoke_preload<ucx::tag_send_sync_nb_tag>(endpoint, buffer, count,
                                                                    datatype, tag, cb);
}
extern "C" ucs_status_ptr_t ucp_tag_recv_nb(ucp_worker_h worker, void* buffer, size_t count,
                                            ucp_datatype_t datatype, ucp_tag_t tag,
                                            ucp_tag_t tag_mask, ucp_tag_recv_callback_t cb) {
    return ucx::Profiler::invoke_preload<ucx::tag_recv_nb_tag>(worker, buffer, count, datatype, tag,
                                                               tag_mask, cb);
}
extern "C" ucp_tag_message_h ucp_tag_probe_nb(ucp_worker_h worker, ucp_tag_t tag,
                                              ucp_tag_t tag_mask, int remove,
                                              ucp_tag_recv_info_t* info) {
    return ucx::Profiler::invoke_preload<ucx::tag_probe_nb_tag>(worker, tag, tag_mask, remove,
                                                                info);
}
extern "C" ucs_status_ptr_t ucp_tag_msg_recv_nb(ucp_worker_h worker, void* buffer, size_t count,
                                                ucp_datatype_t datatype, ucp_tag_message_h message,
                                                ucp_tag_recv_callback_t cb) {
    return ucx::Profiler::invoke_preload<ucx::tag_msg_recv_nb_tag>(worker, buffer, count, datatype,
                                                                   message, cb);
}
extern "C" ucs_status_t ucp_atomic_post(ucp_ep_h endpoint, ucp_atomic_post_op_t op, uint64_t value,
                                        size_t op_size, uint64_t remote_addr, ucp_rkey_h rkey) {
    return ucx::Profiler::invoke_preload<ucx::atomic_post_tag>(endpoint, op, value, op_size,
                                                               remote_addr, rkey);
}
extern "C" ucs_status_ptr_t ucp_atomic_fetch_nb(ucp_ep_h endpoint, ucp_atomic_fetch_op_t op,
                                                uint64_t value, void* result, size_t op_size,
                                                uint64_t remote_addr, ucp_rkey_h rkey,
                                                ucp_send_callback_t cb) {
    return ucx::Profiler::invoke_preload<ucx::atomic_fetch_nb_tag>(endpoint, op, value, result,
                                                                   op_size, remote_addr, rkey, cb);
}
extern "C" ucs_status_ptr_t ucp_atomic_op_nbx(ucp_ep_h endpoint, ucp_atomic_op_t op,
                                              const void* buffer, size_t count,
                                              uint64_t remote_addr, ucp_rkey_h rkey,
                                              const ucp_request_param_t* param) {
    return ucx::Profiler::invoke_preload<ucx::atomic_op_nbx_tag>(endpoint, op, buffer, count,
                                                                 remote_addr, rkey, param);
}

namespace comshu::providers {
class UcxProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"ucx", core::id_range::ucx_lo, core::id_range::ucx_hi};
    }
    bool init(const provider::ProviderConfig&, rec::Recorder<>&) override {
        ::ucx::Profiler::instance();
        return true;
    }
    void shutdown() override {
        comshu_engine_drain_thread();
        comshu_engine_release();
    }
};
}  // namespace comshu::providers

COMSHU_EXPORT_PROVIDER(comshu::providers::UcxProvider)

static void __attribute__((constructor)) comshu_ucx_load() {
    ucx::Profiler::instance();
}
static void __attribute__((destructor)) comshu_ucx_unload() {
    comshu_engine_drain_thread();
    comshu_engine_release();
}
