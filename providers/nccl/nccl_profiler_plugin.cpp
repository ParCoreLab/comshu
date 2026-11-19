// The NCCL profiler plugin door — what NCCL DECIDED.
//
// The interposers in nccl_provider.cpp record the call the application made and
// return before NCCL has chosen anything. Ring or Tree, Simple or LL128, how
// many channels, and the decomposition into per-peer proxy operations are all
// picked inside the enqueue from measured bandwidth and topology, after that
// return. This file is the other end: NCCL calls back into us from inside its
// own machinery and states those decisions.
//
// HOW NCCL FINDS THIS CODE. It is the SAME .so. NCCL passes NCCL_PROFILER_PLUGIN
// straight to dlopen when the value contains a '/'
// (reference/nccl-git/src/plugin/plugin_open.cc:79-95 — openPluginLib tries the
// literal string first and only mangles it into libnccl-profiler-<name>.so if it
// has no slash). libnccl_trace.so is already LD_PRELOADed, so pointing the
// variable at its absolute path makes NCCL dlopen a library that is already
// resident: one process, one recorder, and the plugin callbacks share the
// interposer's state instead of guessing at it. NCCL's matching dlclose only
// drops the reference it took.
//
// WHY THIS IS NOT INFERENCE. Every descriptor carries a `parentObj`, and that
// pointer is a handle WE returned from startEvent for the parent event. The
// chain from a collective to its per-peer proxy operations is NCCL's own
// bookkeeping handed back to us, not something reconstructed here from timing or
// size. That is the whole reason the door is worth opening — the alternative,
// reading ibverbs traffic and guessing which collective produced it, cannot be
// made correct when the same call may move bytes by proxy, by GIN, or by NVLink
// DMA depending on decisions we never see.
//
// WHAT IT STILL CANNOT SEE is stated on the records in nccl_records.hpp, and
// the short version is: device-initiated traffic never reaches the CPU proxy, so
// no proxy op describes it.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string_view>
#include <vector>

#include "comshu/core/records.hpp"
#include "comshu/engine_abi.h"  // comshu_engine_note_cuda_live — see prof_init
#include "comshu/helpers/timestamp.hpp"
#include "comshu/rec/intern_table.hpp"
#include "comshu/rec/recorder.hpp"

#include "nccl_event_pool.hpp"
#include "nccl_profiler_v4.h"
#include "nccl_records.hpp"

namespace nccl_prof {

namespace rr = comshu::providers::nccl::records;
using comshu::rec::InternDomain;

static comshu::rec::Recorder<>& rec() {
    static comshu::rec::Recorder<> proxy;
    return proxy;
}

static std::uint64_t now() { return comshu::helpers::ts::RdtscTimestampSource::Now(); }

static comshu::core::Temporal span(std::uint64_t begin, std::uint64_t end) {
    return comshu::core::Temporal::interval({comshu::core::kDomainHostTsc, begin},
                                           {comshu::core::kDomainHostTsc, end});
}

// NCCL's strings are pointers into libnccl's own constant data
// (ncclAlgoToString and friends), valid for the process lifetime. They are
// interned rather than copied, so "Ring" costs one dictionary entry for the
// whole run and not one per collective.
//
// A null is interned as the empty string rather than skipped, because a Handle
// of 0 and a Handle naming "" are different facts: the first says the field was
// never populated, the second says NCCL gave us nothing for it.
static comshu::core::Handle str(const char* s) {
    return s ? rec().intern(InternDomain::String, std::string_view{s}) : comshu::core::kNullHandle;
}

// --- event identity ---------------------------------------------------------
//
// NCCL hands the pointer we return from startEvent back to us as the `parentObj`
// of every child event. So the handle IS the linkage; all we add is a stable
// number for it that survives into the capture, because a pointer is meaningless
// once the process exits and is reused freely while it runs.
//
// The counter starts at 1. Zero is reserved to mean "parent not observed", which
// happens whenever a parent's event family is masked off — see kDefaultMask
// below. A row saying 0 is saying "NCCL named a parent we were not shown", and
// that is different from "this event is a root".
static std::uint64_t next_event_id() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// Per-communicator state, allocated in init() and handed back to us as
// `context` on every startEvent for that communicator.
struct ProfComm {
    std::uint64_t comm_hash;
};

enum EventKind : std::uint8_t {
    kIgnored = 0,  // a family we were handed but do not record
    kColl = 1,
    kProxyOp = 2,
};

static EventPool& pool() {
    // Never destroyed, deliberately. NCCL's proxy thread can call in during
    // teardown, and a pool whose destructor has already run is the failure the
    // UCX endpoint map's comment in nccl_provider.cpp describes.
    static auto* p = new EventPool();
    return *p;
}

// The id NCCL's parentObj refers to, or 0 when that handle names a slot that has
// since been re-issued — see EventPool for why 0 is the right answer there and
// a plausible id is not.
static std::uint64_t parent_id_of(void* parent_obj) {
    return pool().id_of(parent_obj);
}

// --- the activation mask ----------------------------------------------------
//
// NCCL only GENERATES the event families the plugin asks for, so this is not a
// filter — it is the cost control. ProxyStep fires once per chunk per step and
// would dwarf everything else in the capture; KernelCh fires per channel per
// collective. Neither is enabled by default, and turning them on later is a
// one-line change, whereas discovering their cost on a 16-rank job is a wasted
// allocation.
//
// Group is deliberately absent too. NCCL starts a group event anyway whenever
// any of Coll/P2p/ProxyOp is live (reference/nccl-git/src/plugin/profiler.cc:456)
// and we return an ignored handle for it, which costs one allocation per plan
// and gives the proxy ops a parent chain that terminates cleanly.
static constexpr int kDefaultMask =
    comshuNcclProfileColl | comshuNcclProfileP2p | comshuNcclProfileProxyOp;

static int activation_mask() {
    const char* env = std::getenv("COMSHU_NCCL_PROF_MASK");
    if (!env || !*env)
        return kDefaultMask;
    char*             end = nullptr;
    const long        v = std::strtol(env, &end, 0);
    if (end == env || v <= 0)
        return kDefaultMask;
    return static_cast<int>(v);
}

// --- plugin entry points ----------------------------------------------------

// ARGUMENT ORDER IS v4's, and it is not v5's: v4 is
// (context, mask, commName, commHash, ...) where v5 is
// (context, commId, mask, commName, ...). Both are pointers and scalars in an
// order the compiler cannot check against a struct of function pointers, so
// getting this wrong compiles cleanly and reads the mask pointer out of a
// uint64. See nccl_profiler_v4.h.
static int prof_init(void** context, int* eActivationMask, const char* commName,
                     std::uint64_t commHash, int nNodes, int nranks, int rank,
                     comshuNcclDebugLogger_t) {
    if (eActivationMask)
        *eActivationMask = activation_mask();

    auto* pc = new (std::nothrow) ProfComm{commHash};
    if (context)
        *context = pc;

    // NCCL is BUILDING a communicator right now, so the CUDA driver is live —
    // and this door reaches a capture the interposer's one cannot. NCCL dlopens
    // this library for the plugin even when it was never LD_PRELOADed, so on a
    // `-p ib` run of a NCCL application the plugin fires while ncclCommInitRank
    // is not interposed at all: measured on du04, that capture carried 912/914
    // rows and no 900. That is exactly 02_nccl_internode, which captures a CUDA
    // workload with -p ib and has never had a gpu_device row.
    ::comshu_engine_note_cuda_live("nccl_prof_init");

    // An Artifact: the communicator exists and NCCL calls it this. The temporal
    // is an instant rather than an interval because init is a point in the
    // communicator's construction, not its lifetime — nccl_comm already carries
    // the lifetime, and claiming one here would be a second, different answer to
    // the same question.
    rec().record(rec().now_instant(),
                 rr::NcclProfComm{{},
                                  {},
                                  str(commName),
                                  commHash,
                                  nNodes,
                                  nranks,
                                  rank,
                                  0});
    return 0;
}

static int prof_start_event(void* context, void** eHandle,
                            comshuNcclProfilerEventDescr_v4_t* eDescr) {
    if (eHandle)
        *eHandle = nullptr;
    if (!eHandle || !eDescr)
        return 0;

    void*  handle = nullptr;
    Event* ev = pool().acquire(&handle);
    if (!ev)
        return 0;  // NCCL tolerates a null handle; a dropped event beats a crash
    // The slot is recycled, so every payload field is written here — a stale
    // value from the event that used to live in it would be a plausible number
    // in the wrong row, which is worse than a missing one.
    ev->id.store(next_event_id(), std::memory_order_relaxed);
    ev->func = ev->algo = ev->proto = ev->datatype = comshu::core::kNullHandle;
    ev->seq_number = ev->count = 0;
    ev->root = ev->peer = ev->n_steps = ev->chunk_size = ev->pid = 0;
    ev->n_channels = ev->n_warps = 0;
    ev->channel_id = ev->is_send = 0;
    ev->parent = parent_id_of(eDescr->parentObj);
    ev->begin = now();
    ev->comm_hash = context ? static_cast<ProfComm*>(context)->comm_hash : 0;
    ev->kind = kIgnored;

    switch (eDescr->type) {
    case comshuNcclProfileColl:
        ev->kind = kColl;
        ev->func = str(eDescr->coll.func);
        ev->algo = str(eDescr->coll.algo);
        ev->proto = str(eDescr->coll.proto);
        ev->datatype = str(eDescr->coll.datatype);
        ev->seq_number = eDescr->coll.seqNumber;
        ev->count = eDescr->coll.count;
        ev->root = eDescr->coll.root;
        ev->n_channels = eDescr->coll.nChannels;
        ev->n_warps = eDescr->coll.nWarps;
        // parentObj alone is right here, and it is right on BOTH NCCL versions
        // for different reasons. On a native v4 NCCL the coll's parent IS the
        // plan's group event. On 2.29 the hierarchy moved — parentObj there is
        // the collApi event — but the down-shim overwrites it with
        // coll.parentGroup before we see it
        // (reference/nccl-git/src/plugin/profiler/profiler_v4.cc:37), so a v4
        // plugin gets the group on both. There is no second field to consult.
        break;
    case comshuNcclProfileProxyOp:
        ev->kind = kProxyOp;
        ev->peer = eDescr->proxyOp.peer;
        ev->n_steps = eDescr->proxyOp.nSteps;
        ev->chunk_size = eDescr->proxyOp.chunkSize;
        ev->pid = static_cast<std::int32_t>(eDescr->proxyOp.pid);
        ev->channel_id = eDescr->proxyOp.channelId;
        ev->is_send = eDescr->proxyOp.isSend ? 1 : 0;
        break;
    default:
        // Group, P2p and anything else: tracked only so that children can name a
        // parent id. Not recorded — nccl_p2p already carries the point-to-point
        // calls from the host-API door, with the buffer and stream this
        // descriptor does not have.
        break;
    }

    *eHandle = handle;
    return 0;
}

static int prof_stop_event(void* eHandle) {
    Event* ev = pool().resolve_live(eHandle);
    if (!ev)
        return 0;  // a handle for a slot already re-issued; nothing to emit
    const std::uint64_t end = now();

    switch (ev->kind) {
    case kColl:
        rec().record(span(ev->begin, end), rr::NcclCollExec{{},
                                                            {},
                                                            ev->func,
                                                            ev->algo,
                                                            ev->proto,
                                                            ev->datatype,
                                                            ev->comm_hash,
                                                            ev->id.load(std::memory_order_relaxed),
                                                            ev->parent,
                                                            ev->seq_number,
                                                            ev->count,
                                                            ev->root,
                                                            ev->n_channels,
                                                            ev->n_warps,
                                                            0});
        break;
    case kProxyOp:
        rec().record(span(ev->begin, end), rr::NcclProxyOp{{},
                                                           {},
                                                           ev->id.load(std::memory_order_relaxed),
                                                           ev->parent,
                                                           ev->peer,
                                                           ev->n_steps,
                                                           ev->chunk_size,
                                                           ev->pid,
                                                           ev->channel_id,
                                                           ev->is_send});
        break;
    default:
        break;
    }
    // The slot goes back on the free list, but the handle NCCL may still be
    // holding for this event stays RESOLVABLE until the slot is re-issued —
    // and once it is, the generation makes the stale handle report 0 rather
    // than name the new occupant. See EventPool.
    pool().release(eHandle);
    return 0;
}

// State transitions carry timing detail (per-step transfer sizes, GPU timers)
// for families we do not enable. Accepted and dropped: returning non-zero would
// make NCCL unload the plugin mid-run, which would end the capture.
static int prof_record_event_state(void*, int, comshuNcclProfilerEventStateArgs_v4_t*) { return 0; }

static int prof_finalize(void* context) {
    delete static_cast<ProfComm*>(context);
    return 0;
}

}  // namespace nccl_prof

// The symbol NCCL dlsym's. Must keep this exact name and must be exported —
// providers/nccl/nccl.map lists it for that reason.
// The braces are a linkage block rather than `extern "C" T x = {...}` on one
// line, which gcc warns is "initialized and declared extern" — that form reads
// as a declaration carrying a definition and the warning is fair.
extern "C" {
comshuNcclProfiler_v4_t ncclProfiler_v4 = {
    "comshu",
    nccl_prof::prof_init,
    nccl_prof::prof_start_event,
    nccl_prof::prof_stop_event,
    nccl_prof::prof_record_event_state,
    nccl_prof::prof_finalize,
};
}
