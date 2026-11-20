// NCCL host-API provider — the collective layer, as the application asked for it.
//
// See nccl_hooks.hpp for why every symbol resolves to its `pnccl` twin, and
// nccl_records.hpp for why this records operations rather than trying to
// attribute NCCL's internal channel traffic.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dlfcn.h>

#include <cuda_runtime_api.h>
#include <nccl.h>

#include "comshu/provider/engine_hold.hpp"
#include "comshu/core/mem_attr.hpp"
#include "comshu/core/records.hpp"
#include "comshu/helpers/timestamp.hpp"
#include "comshu/engine_abi.h"
#include "comshu/provider/provider.hpp"
#include "comshu/rec/intern_table.hpp"
#include "comshu/tracer.hpp"

#include "nccl_comm_hash.hpp"
#include "nccl_event_source.hpp"
#include "nccl_records.hpp"

namespace nccl {
namespace rr = comshu::providers::nccl::records;
using comshu::providers::nccl::nccl_comm_hash;
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

// Per-thread group nesting depth.
//
// Counted here rather than read from NCCL, which keeps it in its own internal
// state. Per-thread because grouping is: NCCL's group depth lives in a
// thread-local, so two threads grouping concurrently are two independent
// nestings and a shared counter would interleave them into nonsense.
//
// A plain int, and that is deliberate — see the UCX endpoint map for what
// happens when non-trivially-destructible state ends up on a path that runs
// during teardown. An int has no destructor to have already run.
#if COMSHU_NCCL_HAS_WINDOW
// The windows whose OPEN this provider recorded, so a deregister closes only
// those. NCCL 2.32 registers windows of its own (ce_coll.cc's CE sync window,
// rma/rma_ce.cc's signals window) through ncclDevrWindowRegisterInGroup, which
// is internal and cannot be interposed, and then releases them through the
// public ncclCommWindowDeregister, which is. Closing those named entities no
// open record created on every rank (du04, NCCL 2.32.6: subjects 111 and 117 in
// case 28). There is no door to find — the creation never crosses an exported
// symbol — so the close is not recorded.
static std::mutex&                     windows_mutex() { static std::mutex m; return m; }
static std::unordered_set<const void*>& windows_open() {
    static std::unordered_set<const void*> s;
    return s;
}
#endif

static int& group_depth() {
    static thread_local int depth = 0;
    return depth;
}

// A Start is recorded at the depth it opens, an End at the depth of the Start
// it closes — so a pair carries one number and not two. NCCL allows nesting and
// only the OUTERMOST End launches, which is the distinction this exists to
// preserve.
//
// The End is clamped at 0 rather than allowed to underflow: an unbalanced
// ncclGroupEnd is the application's bug, and wrapping to four billion would
// turn it into ours.
static void record_group(rr::GroupEdge edge, std::uint64_t begin, std::uint64_t end) {
    int&          d = group_depth();
    std::uint32_t at;
    if (edge == rr::GroupEdge::Start) {
        at = static_cast<std::uint32_t>(++d);
    } else {
        at = static_cast<std::uint32_t>(d);
        if (d > 0)
            --d;
    }
    rec().record(span(begin, end), rr::NcclGroup{{}, {}, at, static_cast<std::uint32_t>(edge)});
}

// FNV-1a over the whole ncclUniqueId. Same construction as
// ucx_worker_address's, and chosen for the same property: deterministic
// everywhere, no process salt, so the value is equal on every rank that joined
// this communicator. That equality IS the cross-rank join — a ncclComm_t is a
// local pointer and says nothing about who else is in the group.
static std::uint64_t fnv1a(const void* data, std::size_t len) {
    const auto*   bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Ask NCCL what it just built. These are public calls with pnccl twins, so
// asking is cheap and exact — much better than recording the arguments we were
// handed, because ncclCommInitRankConfig can be given a config that changes the
// effective rank layout, and because a communicator built by ncclCommSplit (a
// door not hooked here) would otherwise carry nothing at all.
//
// `nccl_hash` is NCCL's own commHash, recomputed from the same ncclUniqueId (see
// nccl_comm_hash.hpp). It is passed in rather than derived here because the two
// init doors below are the only places an ncclUniqueId is visible, and a
// communicator that reaches this function by any other route must record 0 —
// "not derivable" — rather than a hash of bytes it never saw.
static void record_comm(ncclComm_t comm, std::uint64_t comm_hash, std::uint64_t nccl_hash,
                        std::uint64_t begin, std::uint64_t end) {
    if (!comm)
        return;
    int rank = -1, nranks = -1, dev = -1;
    pncclCommUserRank(comm, &rank);
    pncclCommCount(comm, &nranks);
    pncclCommCuDevice(comm, &dev);
    // NCCL cannot have built this communicator without a live CUDA context, and
    // has just named the device it sits on — so the driver answers here. That
    // matters because an nccl-only capture has no other moment: it loads no
    // CUPTI, and its host-API records are far too few to fill a record buffer
    // while the process is alive, which is the proxy gpu_device used to depend
    // on. Measured on job 45406132: nccl_internode and nccl_collectives carried
    // no gpu_device at all. See comshu_engine_note_cuda_live.
    ::comshu_engine_note_cuda_live("nccl_comm_init");
    rec().record(span(begin, end),
                 rr::NcclComm{{}, {}, h(comm), comm_hash, nccl_hash, rank, nranks, dev, 0});
}

// --- non-blocking communicators (CR-25) ---------------------------------------
//
// A communicator created with config.blocking = 0 answers its calls with
// ncclInProgress, not ncclSuccess, for work that WAS issued. Testing for
// ncclSuccess alone dropped every collective and P2P such a communicator made,
// and never closed a group: an ncclGroupEnd returning ncclInProgress left
// group_depth one too high for the rest of the thread.
static bool issued(ncclResult_t r) {
#ifdef NCCL_VERSION_CODE
#if NCCL_VERSION_CODE >= NCCL_VERSION(2, 14, 0)
    return r == ncclSuccess || r == ncclInProgress;
#endif
#endif
    return r == ncclSuccess;
}

// A non-blocking INIT is the exception: ncclInProgress there hands back a
// communicator whose rank, size and device NCCL is still filling in on another
// thread. Recording it then would write those placeholders as facts. So it is
// held here, and recorded — with its init call's timing — the first time a
// later call on it finds ncclCommGetAsyncError reporting ready. One that never
// becomes ready is never recorded, and so never closed either.
struct PendingComm {
    std::uint64_t comm_hash, nccl_hash, begin, end;
};
static std::mutex& pending_mutex() {
    static auto* m = new std::mutex();
    return *m;
}
static std::unordered_map<ncclComm_t, PendingComm>& pending_comms() {
    static auto* m = new std::unordered_map<ncclComm_t, PendingComm>();
    return *m;
}
static std::atomic<int> g_pending_count{0};  // the hot path's only cost

static void record_comm(ncclComm_t comm, std::uint64_t comm_hash, std::uint64_t nccl_hash,
                        std::uint64_t begin, std::uint64_t end);

enum class Settled { NotPending, Recorded, StillPending, Failed };

static Settled settle(ncclComm_t comm) {
    if (!comm || g_pending_count.load(std::memory_order_acquire) == 0)
        return Settled::NotPending;
    PendingComm p{};
    {
        std::lock_guard<std::mutex> lk(pending_mutex());
        auto                        it = pending_comms().find(comm);
        if (it == pending_comms().end())
            return Settled::NotPending;
        ncclResult_t state = ncclSuccess;
        if (pncclCommGetAsyncError(comm, &state) != ncclSuccess)
            state = ncclInternalError;
        if (!issued(state) || state != ncclSuccess) {
            if (issued(state))
                return Settled::StillPending;  // ncclInProgress: not yet
            pending_comms().erase(it);         // init failed: nothing to record
            g_pending_count.fetch_sub(1, std::memory_order_release);
            return Settled::Failed;
        }
        p = it->second;
        pending_comms().erase(it);
        g_pending_count.fetch_sub(1, std::memory_order_release);
    }
    record_comm(comm, p.comm_hash, p.nccl_hash, p.begin, p.end);
    return Settled::Recorded;
}

static void comm_init_returned(ncclResult_t res, ncclComm_t comm, std::uint64_t comm_hash,
                               std::uint64_t nccl_hash, std::uint64_t begin, std::uint64_t end) {
    if (!comm)
        return;
    if (res == ncclSuccess) {
        record_comm(comm, comm_hash, nccl_hash, begin, end);
    } else if (issued(res)) {
        std::lock_guard<std::mutex> lk(pending_mutex());
        if (pending_comms().emplace(comm, PendingComm{comm_hash, nccl_hash, begin, end}).second)
            g_pending_count.fetch_add(1, std::memory_order_release);
    }
}

#if COMSHU_NCCL_HAS_WINDOW
// --- windows NCCL has not finished registering (CR-25) -----------------------
//
// ncclCommWindowRegister does not hand back a window. It sets *win = nullptr,
// queues the registration on the group machinery and returns; NCCL writes the
// handle through the caller's pointer when the group completes
// (reference/nccl-git/src/dev_runtime.cc, ncclCommWindowRegister: outWinDev =
// win). Whether that has happened by the time the call returns depends on two
// things, and the old hooks recorded only the case where neither applies:
//
//   - inside the caller's ncclGroupStart/End, the group does not run until the
//     OUTERMOST ncclGroupEnd. The call returns ncclSuccess with *win still
//     null, on a blocking communicator too, so such a window was never
//     recorded at all;
//   - on a non-blocking communicator the group runs on NCCL's own thread and
//     the call (or the group end) returns ncclInProgress. *win, and the peer
//     mappings ncclGetPeerDevicePointer reports, are valid only once
//     ncclCommGetAsyncError says ncclSuccess.
//
// So a registration that has not produced its window is held, with the call's
// timing and the caller's handle slot, and recorded when NCCL is done:
//
//   in a group      on this thread's list, looked at by the outermost
//                   ncclGroupEnd. A communicator ready then is definitive
//                   (handle or no handle); one still in progress moves to
//                   the shared list.
//   in progress     on the shared list, looked at the first time the
//                   communicator is seen ready: the owner's
//                   ncclCommGetAsyncError poll, or any later hooked call on it.
//
// The slot is the CALLER's memory. NCCL itself writes it after returning, so a
// correct program keeps it alive until the communicator reports ready, and the
// first ready observation is where it is read. A window still pending when its
// communicator is destroyed is dropped without a read.
struct PendingWindow {
    ncclComm_t    comm;
    ncclWindow_t* slot;
    void*         buff;
    std::size_t   size;
    int           flags;
    std::uint64_t begin, end;
};

static void record_window(ncclComm_t c, void* buff, std::size_t size, ncclWindow_t win, int flags,
                          std::uint64_t begin, std::uint64_t end) {
    {
        std::lock_guard<std::mutex> lk(windows_mutex());
        if (!windows_open().insert(win).second)
            return;  // already recorded: one open per window
    }
    rec().record(span(begin, end), rr::NcclWindow{{},
                                                  {},
                                                  h(win),
                                                  h(c),
                                                  h(buff),
                                                  static_cast<std::uint64_t>(size),
                                                  flags,
                                                  0,
                                                  reinterpret_cast<std::uint64_t>(buff)});
    // ...and the buffer itself, in the shared vocabulary, so a put placed into
    // this window lands in an OBJECT. Interned on the base address in the
    // Pointer domain like every other emitter, so if gpumem or ibverbs saw the
    // same buffer the rows merge rather than double.
    if (buff) {
        const auto attr = comshu::core::attribute(buff);
        rec().record(rec().now_instant(),
                     comshu::core::records::MemObject{{},
                                                      {},
                                                      h(buff),
                                                      comshu::core::kNullHandle,
                                                      {reinterpret_cast<std::uint64_t>(buff),
                                                       static_cast<std::uint64_t>(size)},
                                                      attr.device,
                                                      attr.status,
                                                      comshu::core::ObjectOrigin::NcclWindow});
    }
    // ...and where every PEER's copy of it sits in this process, which is what
    // lets a store through ncclGetLsaPointer be placed in the owner's buffer
    // (see NcclWindowPeer). Asked of NCCL's public host API, resolved at run
    // time because the provider may be built against headers older than 2.29,
    // where the call does not exist: then there are no rows, which the reader
    // reports as unplaced stores rather than guessing.
    using PeerPtrFn = ncclResult_t (*)(ncclWindow_t, size_t, int, void**);
    static const auto peer_ptr =
        reinterpret_cast<PeerPtrFn>(dlsym(RTLD_DEFAULT, "ncclGetPeerDevicePointer"));
    if (!peer_ptr)
        return;
    int self = -1, nranks = 0;
    if (pncclCommUserRank(c, &self) != ncclSuccess || pncclCommCount(c, &nranks) != ncclSuccess)
        return;
    for (int p = 0; p < nranks; ++p) {
        if (p == self)
            continue;  // our own window: nccl_window.base already says where
        void* q = nullptr;
        // A peer outside this process's LSA team (another node) has no
        // mapping here: NCCL returns an error or a null, and that peer's
        // traffic crosses the network, where the verbs records place it.
        if (peer_ptr(win, 0, p, &q) != ncclSuccess || !q)
            continue;
        rec().record(rec().now_instant(),
                     rr::NcclWindowPeer{{},
                                        {},
                                        h(win),
                                        p,
                                        reinterpret_cast<std::uint64_t>(q),
                                        static_cast<std::uint64_t>(size)});
    }
}

enum class CommState { Ready, InProgress, Failed };
static CommState comm_state(ncclComm_t c) {
    ncclResult_t state = ncclSuccess;
    if (pncclCommGetAsyncError(c, &state) != ncclSuccess)
        return CommState::Failed;
    if (state == ncclSuccess)
        return CommState::Ready;
    return issued(state) ? CommState::InProgress : CommState::Failed;
}

// Leaked, like pending_comms: reachable from hooks that run during teardown.
static std::mutex& async_windows_mutex() {
    static auto* m = new std::mutex();
    return *m;
}
static std::vector<PendingWindow>& async_windows() {
    static auto* v = new std::vector<PendingWindow>();
    return *v;
}
static std::atomic<int> g_async_windows{0};  // the hot path's only cost

// Per thread, because grouping is (see group_depth). A leaked pointer rather
// than a thread_local vector: no destructor to have run by the time a late
// hook on this thread looks at it.
static std::vector<PendingWindow>& group_windows() {
    static thread_local std::vector<PendingWindow>* v = nullptr;
    if (!v)
        v = new std::vector<PendingWindow>();
    return *v;
}

// The shared list: record the entries whose communicator is ready (all of
// them, or only `comm`'s), drop the ones whose communicator failed. Recorded
// outside the lock, which record_window's NCCL queries do not need.
static void settle_windows(ncclComm_t comm) {
    if (g_async_windows.load(std::memory_order_acquire) == 0)
        return;
    std::vector<PendingWindow> ready;
    {
        std::lock_guard<std::mutex> lk(async_windows_mutex());
        auto&                       v = async_windows();
        for (std::size_t i = 0; i < v.size();) {
            const PendingWindow& p = v[i];
            const CommState      st =
                (comm && p.comm != comm) ? CommState::InProgress : comm_state(p.comm);
            if (st == CommState::InProgress) {
                ++i;
                continue;
            }
            if (st == CommState::Ready)
                ready.push_back(p);
            v[i] = v.back();
            v.pop_back();
            g_async_windows.fetch_sub(1, std::memory_order_release);
        }
    }
    for (const PendingWindow& p : ready)
        if (ncclWindow_t w = *p.slot)
            record_window(p.comm, p.buff, p.size, w, p.flags, p.begin, p.end);
}

// A destroyed communicator's windows can no longer become ready.
static void forget_windows(ncclComm_t comm) {
    if (g_async_windows.load(std::memory_order_acquire) == 0)
        return;
    std::lock_guard<std::mutex> lk(async_windows_mutex());
    auto&                       v = async_windows();
    for (std::size_t i = 0; i < v.size();) {
        if (v[i].comm != comm) {
            ++i;
            continue;
        }
        v[i] = v.back();
        v.pop_back();
        g_async_windows.fetch_sub(1, std::memory_order_release);
    }
}

static void hold_async(const PendingWindow& p) {
    std::lock_guard<std::mutex> lk(async_windows_mutex());
    async_windows().push_back(p);
    g_async_windows.fetch_add(1, std::memory_order_release);
}

// The outermost ncclGroupEnd has returned: every registration this thread made
// inside the group has either completed (blocking communicator: a handle, or
// none because NCCL declined) or been handed to NCCL's thread (in progress).
static void close_group_windows() {
    auto& v = group_windows();
    if (v.empty())
        return;
    std::vector<PendingWindow> mine;
    mine.swap(v);
    for (const PendingWindow& p : mine) {
        switch (comm_state(p.comm)) {
        case CommState::Ready:
            if (ncclWindow_t w = *p.slot)
                record_window(p.comm, p.buff, p.size, w, p.flags, p.begin, p.end);
            break;
        case CommState::InProgress:
            hold_async(p);
            break;
        case CommState::Failed:
            break;
        }
    }
}

static void window_register_returned(ncclResult_t res, ncclComm_t c, void* buff, size_t size,
                                     ncclWindow_t* win, int flags, std::uint64_t begin,
                                     std::uint64_t end) {
    if (!issued(res) || !win)
        return;
    settle(c);
    if (res == ncclSuccess && *win) {
        record_window(c, buff, size, *win, flags, begin, end);
        return;
    }
    const PendingWindow p{c, win, buff, size, flags, begin, end};
    if (group_depth() > 0)
        group_windows().push_back(p);
    else if (res != ncclSuccess)
        hold_async(p);
    // else: ncclSuccess, no handle, no group — this communicator has no
    // symmetric memory and NCCL registered nothing (dev_runtime.cc returns
    // before queueing when !comm->symmetricSupport).
}
#else
static void settle_windows(ncclComm_t) {}
static void forget_windows(ncclComm_t) {}
static void close_group_windows() {}
#endif

// Everything a communicator owes the capture once it is ready.
static void settle_all(ncclComm_t comm) {
    settle(comm);
    settle_windows(comm);
}

static void record_coll(rr::CollOp op, const void* sendbuff, void* recvbuff, std::size_t count,
                        ncclDataType_t datatype, int redop, int root, ncclComm_t comm,
                        cudaStream_t stream, std::uint64_t begin, std::uint64_t end) {
    settle_all(comm);
    rec().record(span(begin, end), rr::NcclCollective{{},
                                                      {},
                                                      h(comm),
                                                      h(sendbuff),
                                                      h(recvbuff),
                                                      h(stream),
                                                      static_cast<std::uint64_t>(count),
                                                      root,
                                                      static_cast<std::uint8_t>(op),
                                                      static_cast<std::uint8_t>(datatype),
                                                      static_cast<std::uint8_t>(redop),
                                                      0});
}

Profiler::Profiler() {
    const comshu::Config cfg = comshu::Config::from_env();
    if (comshu_engine_declare_provider("nccl", comshu::core::id_range::nccl_lo,
                                       comshu::core::id_range::nccl_hi,
                                       COMSHU_ENGINE_ABI_VERSION) != 0)
        return;
    ::comshu::provider::engine_hold().acquire(cfg.output_dir.c_str(), cfg.rank,
                                                 cfg.verbose ? 1 : 0);

    // --- communicator lifecycle ---------------------------------------------
    register_post<comm_init_rank_tag>(
        [](ncclResult_t res, ncclComm_t* comm, int, ncclUniqueId id, int, auto begin, auto end) {
            if (comm)
                comm_init_returned(res, *comm, fnv1a(id.internal, sizeof(id.internal)),
                                   nccl_comm_hash(id.internal, sizeof(id.internal)), begin, end);
        });
    register_post<comm_init_rank_config_tag>([](ncclResult_t res, ncclComm_t* comm, int,
                                                ncclUniqueId id, int, ncclConfig_t*, auto begin,
                                                auto end) {
        if (comm)
            comm_init_returned(res, *comm, fnv1a(id.internal, sizeof(id.internal)),
                               nccl_comm_hash(id.internal, sizeof(id.internal)), begin, end);
    });
    // PRE, not post: after the call the communicator is gone and interning its
    // pointer would name a dead object — and worse, the allocator is free to
    // hand the same address to the next comm, which is precisely the reuse the
    // close-accounting assertion is built to notice.
    //
    // Destroy and abort are ALTERNATIVES, not nested doors, so neither is
    // suppressed: ncclCommDestroy reaches commReclaim rather than ncclCommAbort
    // (reference/nccl-git/src/init.cc:2879). If some stack does nest them, the
    // result is a "more closes than opens" row — the assertion doing its job,
    // exactly as it did for UCX endpoints — and that is the signal we want
    // rather than a pre-emptive filter over a case we have not observed.
    // A communicator still pending here was never recorded, so it gets no
    // close either (CR-25).
    register_pre<comm_destroy_tag>([](ncclComm_t comm, auto) {
        settle_windows(comm);
        forget_windows(comm);
        const Settled st = settle(comm);
        if (st == Settled::StillPending || st == Settled::Failed)
            return;
        close(comm, comshu::core::RecordTrait<rr::NcclComm>::type_id);
    });
    register_pre<comm_abort_tag>([](ncclComm_t comm, auto) {
        settle_windows(comm);
        forget_windows(comm);
        const Settled st = settle(comm);
        if (st == Settled::StillPending) {
            std::lock_guard<std::mutex> lk(pending_mutex());
            if (pending_comms().erase(comm))
                g_pending_count.fetch_sub(1, std::memory_order_release);
            return;
        }
        if (st == Settled::Failed)
            return;
        close(comm, comshu::core::RecordTrait<rr::NcclComm>::type_id);
    });

    // The owner of a non-blocking communicator polls this until ncclSuccess:
    // the first moment its pending init and windows can be read (CR-25).
    register_post<comm_get_async_error_tag>(
        [](ncclResult_t res, ncclComm_t comm, ncclResult_t* state, auto, auto) {
            if (res == ncclSuccess && state && *state == ncclSuccess)
                settle_all(comm);
        });

    // --- collectives ---------------------------------------------------------
    // `root` is -1 where the operation has none, and `redop` 0xff where it does
    // not reduce. Neither is a zero, because 0 is a valid rank and ncclSum is a
    // valid reduction: a sentinel that collides with real data is how a column
    // starts lying.
    register_post<all_reduce_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                     ncclDataType_t dt, ncclRedOp_t op, ncclComm_t c,
                                     cudaStream_t st, auto begin, auto end) {
        if (issued(res))
            record_coll(rr::CollOp::AllReduce, s, r, n, dt, op, -1, c, st, begin, end);
    });
    register_post<broadcast_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                    ncclDataType_t dt, int root, ncclComm_t c, cudaStream_t st,
                                    auto begin, auto end) {
        if (issued(res))
            record_coll(rr::CollOp::Broadcast, s, r, n, dt, 0xff, root, c, st, begin, end);
    });
    register_post<reduce_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                 ncclDataType_t dt, ncclRedOp_t op, int root, ncclComm_t c,
                                 cudaStream_t st, auto begin, auto end) {
        if (issued(res))
            record_coll(rr::CollOp::Reduce, s, r, n, dt, op, root, c, st, begin, end);
    });
    // AllGather's count is the SEND count and ReduceScatter's is the RECV count
    // — each is the per-rank slice, and the other side of the operation is
    // nranks times bigger. The record stores what the API was given and the
    // CollOp says which of the two it is; normalising them to one "size" here
    // would make the two ops indistinguishable in exactly the field a reader
    // would use to size an edge.
    register_post<all_gather_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                     ncclDataType_t dt, ncclComm_t c, cudaStream_t st, auto begin,
                                     auto end) {
        if (issued(res))
            record_coll(rr::CollOp::AllGather, s, r, n, dt, 0xff, -1, c, st, begin, end);
    });
    register_post<reduce_scatter_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                         ncclDataType_t dt, ncclRedOp_t op, ncclComm_t c,
                                         cudaStream_t st, auto begin, auto end) {
        if (issued(res))
            record_coll(rr::CollOp::ReduceScatter, s, r, n, dt, op, -1, c, st, begin, end);
    });

    // --- point to point ------------------------------------------------------
    register_post<send_tag>([](ncclResult_t res, const void* buf, size_t n, ncclDataType_t dt,
                               int peer, ncclComm_t c, cudaStream_t st, auto begin, auto end) {
        if (!issued(res))
            return;
        settle_all(c);
        rec().record(span(begin, end), rr::NcclP2p{{},
                                                       {},
                                                       h(c),
                                                       h(buf),
                                                       h(st),
                                                       static_cast<std::uint64_t>(n),
                                                       peer,
                                                       static_cast<std::uint8_t>(rr::P2pDir::Send),
                                                       static_cast<std::uint8_t>(dt),
                                                       0});
    });
    register_post<recv_tag>([](ncclResult_t res, void* buf, size_t n, ncclDataType_t dt, int peer,
                               ncclComm_t c, cudaStream_t st, auto begin, auto end) {
        if (!issued(res))
            return;
        settle_all(c);
        rec().record(span(begin, end), rr::NcclP2p{{},
                                                       {},
                                                       h(c),
                                                       h(buf),
                                                       h(st),
                                                       static_cast<std::uint64_t>(n),
                                                       peer,
                                                       static_cast<std::uint8_t>(rr::P2pDir::Recv),
                                                       static_cast<std::uint8_t>(dt),
                                                       0});
    });

#if COMSHU_NCCL_HAS_WINDOW
    // --- symmetric memory ----------------------------------------------------
    // Recorded when NCCL has written the handle back, which is not always by
    // the time this returns: see PendingWindow.
    register_post<window_register_tag>([](ncclResult_t res, ncclComm_t c, void* buff, size_t size,
                                          ncclWindow_t* win, int flags, auto begin, auto end) {
        window_register_returned(res, c, buff, size, win, flags, begin, end);
    });
    register_pre<window_deregister_tag>([](ncclComm_t c, ncclWindow_t win, auto) {
        settle_all(c);  // a window still pending is recorded before it closes
        {
            std::lock_guard<std::mutex> lk(windows_mutex());
            if (windows_open().erase(win) == 0)
                return;  // one of NCCL's own windows: we never saw it open
        }
        close(win, comshu::core::RecordTrait<rr::NcclWindow>::type_id);
    });
    register_post<dev_comm_create_tag>(
        [](ncclResult_t res, ncclComm_t c, const void*, void*, auto begin, auto end) {
            if (res == ncclSuccess)
                rec().record(span(begin, end), rr::NcclDevComm{{}, {}, h(c), 0});
        });
#endif
}

template class NcclEventSource<Profiler>;

}  // namespace nccl

// The interposers. Each is a STRONG definition of the public name, which beats
// libnccl's weak one regardless of load order, and forwards to the `pnccl`
// twin — see nccl_hooks.hpp.
extern "C" ncclResult_t ncclCommInitRank(ncclComm_t* comm, int nranks, ncclUniqueId commId,
                                         int rank) {
    return nccl::Profiler::invoke_preload<nccl::comm_init_rank_tag>(comm, nranks, commId, rank);
}
extern "C" ncclResult_t ncclCommInitRankConfig(ncclComm_t* comm, int nranks, ncclUniqueId commId,
                                               int rank, ncclConfig_t* config) {
    return nccl::Profiler::invoke_preload<nccl::comm_init_rank_config_tag>(comm, nranks, commId,
                                                                           rank, config);
}
extern "C" ncclResult_t ncclCommDestroy(ncclComm_t comm) {
    return nccl::Profiler::invoke_preload<nccl::comm_destroy_tag>(comm);
}
extern "C" ncclResult_t ncclCommAbort(ncclComm_t comm) {
    return nccl::Profiler::invoke_preload<nccl::comm_abort_tag>(comm);
}
extern "C" ncclResult_t ncclCommGetAsyncError(ncclComm_t comm, ncclResult_t* asyncError) {
    return nccl::Profiler::invoke_preload<nccl::comm_get_async_error_tag>(comm, asyncError);
}
extern "C" ncclResult_t ncclAllReduce(const void* sendbuff, void* recvbuff, size_t count,
                                      ncclDataType_t datatype, ncclRedOp_t op, ncclComm_t comm,
                                      cudaStream_t stream) {
    return nccl::Profiler::invoke_preload<nccl::all_reduce_tag>(sendbuff, recvbuff, count, datatype,
                                                                op, comm, stream);
}
extern "C" ncclResult_t ncclBroadcast(const void* sendbuff, void* recvbuff, size_t count,
                                      ncclDataType_t datatype, int root, ncclComm_t comm,
                                      cudaStream_t stream) {
    return nccl::Profiler::invoke_preload<nccl::broadcast_tag>(sendbuff, recvbuff, count, datatype,
                                                               root, comm, stream);
}
extern "C" ncclResult_t ncclReduce(const void* sendbuff, void* recvbuff, size_t count,
                                   ncclDataType_t datatype, ncclRedOp_t op, int root,
                                   ncclComm_t comm, cudaStream_t stream) {
    return nccl::Profiler::invoke_preload<nccl::reduce_tag>(sendbuff, recvbuff, count, datatype, op,
                                                            root, comm, stream);
}
extern "C" ncclResult_t ncclAllGather(const void* sendbuff, void* recvbuff, size_t sendcount,
                                      ncclDataType_t datatype, ncclComm_t comm,
                                      cudaStream_t stream) {
    return nccl::Profiler::invoke_preload<nccl::all_gather_tag>(sendbuff, recvbuff, sendcount,
                                                                datatype, comm, stream);
}
extern "C" ncclResult_t ncclReduceScatter(const void* sendbuff, void* recvbuff, size_t recvcount,
                                          ncclDataType_t datatype, ncclRedOp_t op, ncclComm_t comm,
                                          cudaStream_t stream) {
    return nccl::Profiler::invoke_preload<nccl::reduce_scatter_tag>(sendbuff, recvbuff, recvcount,
                                                                    datatype, op, comm, stream);
}
extern "C" ncclResult_t ncclSend(const void* sendbuff, size_t count, ncclDataType_t datatype,
                                 int peer, ncclComm_t comm, cudaStream_t stream) {
    return nccl::Profiler::invoke_preload<nccl::send_tag>(sendbuff, count, datatype, peer, comm,
                                                          stream);
}
extern "C" ncclResult_t ncclRecv(void* recvbuff, size_t count, ncclDataType_t datatype, int peer,
                                 ncclComm_t comm, cudaStream_t stream) {
    return nccl::Profiler::invoke_preload<nccl::recv_tag>(recvbuff, count, datatype, peer, comm,
                                                          stream);
}
// The two nullary entry points, interposed by hand — see nccl_hooks.hpp for why
// the macro cannot express them. They do what the generated path does: stamp
// the call, forward to the p-twin, record. rdtsc directly, because that is the
// clock DefaultHookManagerConfig uses and a group row timed off a different
// source would not be orderable against the collectives around it.
//
// The Profiler is touched first so that a group call arriving before any other
// NCCL call still starts the capture, rather than recording into nothing.
extern "C" ncclResult_t ncclGroupStart() {
    nccl::Profiler::instance();
    const auto   begin = comshu::helpers::ts::RdtscTimestampSource::Now();
    ncclResult_t res = pncclGroupStart();
    const auto   end = comshu::helpers::ts::RdtscTimestampSource::Now();
    if (nccl::issued(res))
        nccl::record_group(nccl::rr::GroupEdge::Start, begin, end);
    return res;
}
extern "C" ncclResult_t ncclGroupEnd() {
    nccl::Profiler::instance();
    const auto   begin = comshu::helpers::ts::RdtscTimestampSource::Now();
    ncclResult_t res = pncclGroupEnd();
    const auto   end = comshu::helpers::ts::RdtscTimestampSource::Now();
    if (nccl::issued(res))
        nccl::record_group(nccl::rr::GroupEdge::End, begin, end);
    // The outermost End ran the group (or failed it): this thread's in-group
    // window registrations are done or handed to NCCL's thread.
    if (nccl::group_depth() == 0 || !nccl::issued(res))
        nccl::close_group_windows();
    return res;
}
#if COMSHU_NCCL_HAS_WINDOW
extern "C" ncclResult_t ncclCommWindowRegister(ncclComm_t comm, void* buff, size_t size,
                                               ncclWindow_t* win, int winFlags) {
    return nccl::Profiler::invoke_preload<nccl::window_register_tag>(comm, buff, size, win,
                                                                     winFlags);
}
extern "C" ncclResult_t ncclCommWindowDeregister(ncclComm_t comm, ncclWindow_t win) {
    return nccl::Profiler::invoke_preload<nccl::window_deregister_tag>(comm, win);
}
extern "C" ncclResult_t ncclDevCommCreate(ncclComm_t comm, const void* reqs, void* outDevComm) {
    return nccl::Profiler::invoke_preload<nccl::dev_comm_create_tag>(comm, reqs, outDevComm);
}
#endif

namespace comshu::providers {
class NcclProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"nccl", core::id_range::nccl_lo, core::id_range::nccl_hi};
    }
    bool init(const provider::ProviderConfig&, rec::Recorder<>&) override {
        ::nccl::Profiler::instance();
        return true;
    }
    void shutdown() override {
        comshu_engine_drain_thread();
        ::comshu::provider::engine_hold().release();
    }
};
}  // namespace comshu::providers

COMSHU_EXPORT_PROVIDER(comshu::providers::NcclProvider)

static void __attribute__((constructor)) comshu_nccl_load() {
    nccl::Profiler::instance();
}
static void __attribute__((destructor)) comshu_nccl_unload() {
    comshu_engine_drain_thread();
    ::comshu::provider::engine_hold().release();
}
