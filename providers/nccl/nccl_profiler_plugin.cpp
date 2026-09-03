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

#include "pcomm/core/records.hpp"
#include "pcomm/helpers/timestamp.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/rec/recorder.hpp"

#include "nccl_profiler_v4.h"
#include "nccl_records.hpp"

namespace nccl_prof {

namespace rr = pcomm::providers::nccl::records;
using pcomm::rec::InternDomain;

static pcomm::rec::Recorder<>& rec() {
    static pcomm::rec::Recorder<> proxy;
    return proxy;
}

static std::uint64_t now() { return pcomm::helpers::ts::RdtscTimestampSource::Now(); }

static pcomm::core::Temporal span(std::uint64_t begin, std::uint64_t end) {
    return pcomm::core::Temporal::interval({pcomm::core::kDomainHostTsc, begin},
                                           {pcomm::core::kDomainHostTsc, end});
}

// NCCL's strings are pointers into libnccl's own constant data
// (ncclAlgoToString and friends), valid for the process lifetime. They are
// interned rather than copied, so "Ring" costs one dictionary entry for the
// whole run and not one per collective.
//
// A null is interned as the empty string rather than skipped, because a Handle
// of 0 and a Handle naming "" are different facts: the first says the field was
// never populated, the second says NCCL gave us nothing for it.
static pcomm::core::Handle str(const char* s) {
    return s ? rec().intern(InternDomain::String, std::string_view{s}) : pcomm::core::kNullHandle;
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

// One in-flight event. Everything is copied out of the descriptor at
// startEvent, because the descriptor is a stack temporary in NCCL's frame and
// is gone by the time stopEvent arrives — only the interned handles and plain
// integers here survive to be emitted.
//
// A tagged struct with a union would be tighter. It is spelled out flat because
// the cost is a few dozen bytes on an object that lives for the duration of one
// collective, and because a union here would be one more place where reading
// the wrong arm produces a plausible number instead of a compile error.
struct Event {
    // Bumped on every acquire. A handle carries the generation it was minted
    // with, so a stale one never matches a re-issued slot. See EventPool.
    std::uint32_t generation;
    std::uint32_t next_free;  // slot index, kNoSlot at the end of the list

    std::uint64_t id;
    std::uint64_t parent;
    std::uint64_t begin;
    std::uint64_t comm_hash;
    std::uint8_t  kind;  // EventKind

    // coll
    pcomm::core::Handle func, algo, proto, datatype;
    std::uint64_t       seq_number;
    std::uint64_t       count;
    std::int32_t        root;
    std::uint8_t        n_channels;
    std::uint8_t        n_warps;

    // proxyOp
    std::int32_t  peer;
    std::int32_t  n_steps;
    std::int32_t  chunk_size;
    std::int32_t  pid;
    std::uint32_t channel_id;
    std::uint32_t is_send;
};

enum EventKind : std::uint8_t {
    kIgnored = 0,  // a family we were handed but do not record
    kColl = 1,
    kProxyOp = 2,
};

// --- the event pool -------------------------------------------------------
//
// THE HANDLE WE GIVE NCCL IS NOT A POINTER. It is (slot index, generation)
// packed into the void*, and that is the entire design.
//
// Why a pointer cannot work. A proxy op's parent is the COLLECTIVE event — NCCL
// sets `eDescr.parentObj = sub->taskEventHandle`
// (reference/nccl-git/src/plugin/profiler.cc:574) — and
// ncclProfilerStartProxyOpEvent runs on the PROXY THREAD while the collective is
// stopped from the application thread. So NCCL routinely hands back a handle for
// an event that has already ended. With `new`/`delete` that is a use-after-free.
// With a recycled pool it is worse: the slot has been reused, and the child
// reads a plausible id belonging to a DIFFERENT event.
//
// That is not hypothetical. It was measured on MN5 job 45350841, case 02:
// proxy op 42's parent came back as 42 — ITSELF — and ops 43 and 44 also named
// 42, because the collective they belonged to had been stopped and its slot
// re-issued to proxy op 42. Confidently wrong attribution, which is the one
// outcome worse than no attribution.
//
// A refcount does not fix this and NVIDIA's example plugin has the same hole
// (plugins/profiler/example/plugin.cc:531 increments `parent->refCount`, which
// cannot help a child that arrives after the count already reached zero).
//
// The generation does fix it, completely. Slots live in blocks that are never
// freed, so an index is always valid; a generation is stamped on every acquire,
// so a handle minted before a recycle carries the OLD generation and can never
// match. The check is exact in both directions:
//
//   generation matches  -> the slot still describes that event, and its id is
//                          correct EVEN IF the event has already stopped. A
//                          stopped-but-not-yet-reused parent is still that
//                          parent, and naming it is right.
//   generation differs   -> the slot was re-issued; report 0, "not observed".
//
// No refcount is needed, which removes the ABA problem rather than defending
// against it.
//
// A FREED SLOT IS NOT REUSED WHILE ANY UNUSED SLOT REMAINS, and that is what
// makes the linkage USEFUL rather than merely safe.
//
// The generation alone is safe: a recycled slot reports 0 instead of naming the
// wrong event. But on the first clean multi-node board (MN5 job 45359727,
// case 02) safe-alone turned every one of 320 proxy ops into parent 0 — correct,
// and useless. NCCL stops a collective long before the proxy ops it spawned
// arrive, and a LIFO free list hands that just-freed slot to the very next
// event, so the parent was always gone by the time a child asked.
//
// So the pool hands out never-used storage first and only falls back to freed
// slots, oldest-first, once it reaches its ceiling. Correctness never rests on
// that ordering — a child arriving after its parent is finally reissued still
// gets 0, never a wrong id — so the delay buys usefulness without being
// load-bearing.
class EventPool {
public:
    // Encoded handle: index in the high 32 bits, generation in the low 32.
    // Generations start at 1 so a valid handle is never null — NCCL tests
    // `if (handle)` before calling stopEvent.
    static void* encode(std::uint32_t index, std::uint32_t gen) {
        return reinterpret_cast<void*>((static_cast<std::uint64_t>(index) << 32) | gen);
    }

    Event* acquire(void** out_handle) {
        std::lock_guard<std::mutex> lk(mu_);
        std::uint32_t               index;
        if (fresh_next_ < count_.load(std::memory_order_relaxed) || grow()) {
            // Never-used storage first: nothing can hold a handle to it, so it
            // cannot disturb anyone's parent lookup.
            index = fresh_next_++;
        } else if (quarantined_ > 0) {
            // At the block ceiling. Reuse the oldest freed slot even though the
            // quarantine is short — the generation still makes a stale handle
            // report 0, so this degrades the linkage window and never the
            // correctness.
            index = pop_oldest();
        } else {
            return nullptr;
        }
        Event*              e = at(index);
        const std::uint32_t gen = e->generation + 1;
        e->generation = gen;
        *out_handle = encode(index, gen);
        return e;
    }

    // Resolve a handle to its event, or nullptr if the slot has been re-issued.
    Event* resolve(void* handle) {
        const auto          raw = reinterpret_cast<std::uint64_t>(handle);
        const std::uint32_t index = static_cast<std::uint32_t>(raw >> 32);
        const std::uint32_t gen = static_cast<std::uint32_t>(raw);
        if (gen == 0 || index >= count_.load(std::memory_order_acquire))
            return nullptr;
        Event* e = at(index);
        return e->generation == gen ? e : nullptr;
    }

    // To the BACK of the queue, never the front. A LIFO free list hands the
    // just-stopped collective straight to the next proxy op that starts, which
    // is exactly the pairing that must not happen.
    void release(void* handle) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!resolve(handle))
            return;
        const auto index = static_cast<std::uint32_t>(reinterpret_cast<std::uint64_t>(handle) >> 32);
        at(index)->next_free = kNoSlot;
        if (tail_ == kNoSlot)
            head_ = index;
        else
            at(tail_)->next_free = index;
        tail_ = index;
        ++quarantined_;
    }

private:
    static constexpr std::uint32_t kBlock = 512;
    static constexpr std::uint32_t kMaxBlocks = 4096;
    static constexpr std::uint32_t kNoSlot = 0xffffffffu;
    // Freed slots are only reissued once the pool runs out of never-used
    // storage, and then oldest-first. With 4096 blocks of 512 that is up to 2M
    // slots before any reuse at all, which is far more than any collective's
    // proxy ops can lag it by.
    static constexpr std::uint32_t kMaxSlots = kBlock * kMaxBlocks;

    std::atomic<Event*>        blocks_[kMaxBlocks]{};
    std::atomic<std::uint32_t> count_{0};       // slots published so far
    std::uint32_t              fresh_next_ = 0; // next never-used slot
    std::uint32_t              head_ = kNoSlot; // oldest freed
    std::uint32_t              tail_ = kNoSlot;
    std::uint32_t              quarantined_ = 0;
    std::mutex                 mu_;

    Event* at(std::uint32_t index) {
        return blocks_[index / kBlock].load(std::memory_order_acquire) + (index % kBlock);
    }

    std::uint32_t pop_oldest() {
        const std::uint32_t index = head_;
        head_ = at(index)->next_free;
        if (head_ == kNoSlot)
            tail_ = kNoSlot;
        --quarantined_;
        return index;
    }

    // Caller holds mu_.
    bool grow() {
        const std::uint32_t published = count_.load(std::memory_order_relaxed);
        const std::uint32_t block = published / kBlock;
        if (block >= kMaxBlocks)
            return false;
        auto* mem = new (std::nothrow) Event[kBlock]();
        if (!mem)
            return false;
        for (std::uint32_t i = 0; i < kBlock; ++i)
            mem[i].generation = 0;
        blocks_[block].store(mem, std::memory_order_release);
        // Published only after the block pointer is visible, so an index below
        // count_ always resolves.
        count_.store(published + kBlock, std::memory_order_release);
        return true;
    }
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
    const Event* e = pool().resolve(parent_obj);
    return e ? e->id : 0;
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
    pcommNcclProfileColl | pcommNcclProfileP2p | pcommNcclProfileProxyOp;

static int activation_mask() {
    const char* env = std::getenv("PCOMM_NCCL_PROF_MASK");
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
                     pcommNcclDebugLogger_t) {
    if (eActivationMask)
        *eActivationMask = activation_mask();

    auto* pc = new (std::nothrow) ProfComm{commHash};
    if (context)
        *context = pc;

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
                            pcommNcclProfilerEventDescr_v4_t* eDescr) {
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
    ev->id = next_event_id();
    ev->func = ev->algo = ev->proto = ev->datatype = pcomm::core::kNullHandle;
    ev->seq_number = ev->count = 0;
    ev->root = ev->peer = ev->n_steps = ev->chunk_size = ev->pid = 0;
    ev->n_channels = ev->n_warps = 0;
    ev->channel_id = ev->is_send = 0;
    ev->parent = parent_id_of(eDescr->parentObj);
    ev->begin = now();
    ev->comm_hash = context ? static_cast<ProfComm*>(context)->comm_hash : 0;
    ev->kind = kIgnored;

    switch (eDescr->type) {
    case pcommNcclProfileColl:
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
    case pcommNcclProfileProxyOp:
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
    Event* ev = pool().resolve(eHandle);
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
                                                            ev->id,
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
                                                           ev->id,
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
static int prof_record_event_state(void*, int, pcommNcclProfilerEventStateArgs_v4_t*) { return 0; }

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
pcommNcclProfiler_v4_t ncclProfiler_v4 = {
    "pcomm",
    nccl_prof::prof_init,
    nccl_prof::prof_start_event,
    nccl_prof::prof_stop_event,
    nccl_prof::prof_record_event_state,
    nccl_prof::prof_finalize,
};
}
