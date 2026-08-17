// NCCL host-API provider — the collective layer, as the application asked for it.
//
// See nccl_hooks.hpp for why every symbol resolves to its `pnccl` twin, and
// nccl_records.hpp for why this records operations rather than trying to
// attribute NCCL's internal channel traffic.

#include <cstdint>

#include <cuda_runtime_api.h>
#include <nccl.h>

#include "pcomm/core/records.hpp"
#include "pcomm/helpers/timestamp.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/tracer.hpp"

#include "nccl_event_source.hpp"
#include "nccl_records.hpp"

namespace nccl {
namespace rr = pcomm::providers::nccl::records;
using pcomm::rec::InternDomain;

static pcomm::rec::Recorder<>& rec() {
    static pcomm::rec::Recorder<> proxy;
    return proxy;
}

static pcomm::core::Handle h(const void* p) {
    return rec().intern(InternDomain::Pointer, reinterpret_cast<std::uint64_t>(p));
}

static pcomm::core::Temporal span(std::uint64_t begin, std::uint64_t end) {
    return pcomm::core::Temporal::interval({pcomm::core::kDomainHostTsc, begin},
                                           {pcomm::core::kDomainHostTsc, end});
}

static void close(const void* p, pcomm::core::RecordTypeId opener) {
    if (p)
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{{}, {}, h(p), opener});
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
static void record_comm(ncclComm_t comm, std::uint64_t comm_hash, std::uint64_t begin,
                        std::uint64_t end) {
    if (!comm)
        return;
    int rank = -1, nranks = -1, dev = -1;
    pncclCommUserRank(comm, &rank);
    pncclCommCount(comm, &nranks);
    pncclCommCuDevice(comm, &dev);
    rec().record(span(begin, end), rr::NcclComm{{}, {}, h(comm), comm_hash, rank, nranks, dev});
}

static void record_coll(rr::CollOp op, const void* sendbuff, void* recvbuff, std::size_t count,
                        ncclDataType_t datatype, int redop, int root, ncclComm_t comm,
                        cudaStream_t stream, std::uint64_t begin, std::uint64_t end) {
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
    const pcomm::Config cfg = pcomm::Config::from_env();
    if (pcomm_engine_declare_provider("nccl", pcomm::core::id_range::nccl_lo,
                                      pcomm::core::id_range::nccl_hi,
                                      PCOMM_ENGINE_ABI_VERSION) != 0)
        return;
    pcomm_engine_acquire(cfg.output_dir.c_str(), cfg.rank, cfg.verbose ? 1 : 0);

    // --- communicator lifecycle ---------------------------------------------
    register_post<comm_init_rank_tag>(
        [](ncclResult_t res, ncclComm_t* comm, int, ncclUniqueId id, int, auto begin, auto end) {
            if (res == ncclSuccess && comm && *comm)
                record_comm(*comm, fnv1a(id.internal, sizeof(id.internal)), begin, end);
        });
    register_post<comm_init_rank_config_tag>([](ncclResult_t res, ncclComm_t* comm, int,
                                                ncclUniqueId id, int, ncclConfig_t*, auto begin,
                                                auto end) {
        if (res == ncclSuccess && comm && *comm)
            record_comm(*comm, fnv1a(id.internal, sizeof(id.internal)), begin, end);
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
    register_pre<comm_destroy_tag>([](ncclComm_t comm, auto) {
        close(comm, pcomm::core::RecordTrait<rr::NcclComm>::type_id);
    });
    register_pre<comm_abort_tag>([](ncclComm_t comm, auto) {
        close(comm, pcomm::core::RecordTrait<rr::NcclComm>::type_id);
    });

    // --- collectives ---------------------------------------------------------
    // `root` is -1 where the operation has none, and `redop` 0xff where it does
    // not reduce. Neither is a zero, because 0 is a valid rank and ncclSum is a
    // valid reduction: a sentinel that collides with real data is how a column
    // starts lying.
    register_post<all_reduce_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                     ncclDataType_t dt, ncclRedOp_t op, ncclComm_t c,
                                     cudaStream_t st, auto begin, auto end) {
        if (res == ncclSuccess)
            record_coll(rr::CollOp::AllReduce, s, r, n, dt, op, -1, c, st, begin, end);
    });
    register_post<broadcast_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                    ncclDataType_t dt, int root, ncclComm_t c, cudaStream_t st,
                                    auto begin, auto end) {
        if (res == ncclSuccess)
            record_coll(rr::CollOp::Broadcast, s, r, n, dt, 0xff, root, c, st, begin, end);
    });
    register_post<reduce_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                 ncclDataType_t dt, ncclRedOp_t op, int root, ncclComm_t c,
                                 cudaStream_t st, auto begin, auto end) {
        if (res == ncclSuccess)
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
        if (res == ncclSuccess)
            record_coll(rr::CollOp::AllGather, s, r, n, dt, 0xff, -1, c, st, begin, end);
    });
    register_post<reduce_scatter_tag>([](ncclResult_t res, const void* s, void* r, size_t n,
                                         ncclDataType_t dt, ncclRedOp_t op, ncclComm_t c,
                                         cudaStream_t st, auto begin, auto end) {
        if (res == ncclSuccess)
            record_coll(rr::CollOp::ReduceScatter, s, r, n, dt, op, -1, c, st, begin, end);
    });

    // --- point to point ------------------------------------------------------
    register_post<send_tag>([](ncclResult_t res, const void* buf, size_t n, ncclDataType_t dt,
                               int peer, ncclComm_t c, cudaStream_t st, auto begin, auto end) {
        if (res == ncclSuccess)
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
        if (res == ncclSuccess)
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

#if PCOMM_NCCL_HAS_WINDOW
    // --- symmetric memory ----------------------------------------------------
    register_post<window_register_tag>([](ncclResult_t res, ncclComm_t c, void* buff, size_t size,
                                          ncclWindow_t* win, int flags, auto begin, auto end) {
        if (res == ncclSuccess && win && *win)
            rec().record(
                span(begin, end),
                rr::NcclWindow{
                    {}, {}, h(*win), h(c), h(buff), static_cast<std::uint64_t>(size), flags, 0});
    });
    register_pre<window_deregister_tag>([](ncclComm_t, ncclWindow_t win, auto) {
        close(win, pcomm::core::RecordTrait<rr::NcclWindow>::type_id);
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
    const auto   begin = pcomm::helpers::ts::RdtscTimestampSource::Now();
    ncclResult_t res = pncclGroupStart();
    const auto   end = pcomm::helpers::ts::RdtscTimestampSource::Now();
    if (res == ncclSuccess)
        nccl::record_group(nccl::rr::GroupEdge::Start, begin, end);
    return res;
}
extern "C" ncclResult_t ncclGroupEnd() {
    nccl::Profiler::instance();
    const auto   begin = pcomm::helpers::ts::RdtscTimestampSource::Now();
    ncclResult_t res = pncclGroupEnd();
    const auto   end = pcomm::helpers::ts::RdtscTimestampSource::Now();
    if (res == ncclSuccess)
        nccl::record_group(nccl::rr::GroupEdge::End, begin, end);
    return res;
}
#if PCOMM_NCCL_HAS_WINDOW
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

namespace pcomm::providers {
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
        pcomm_engine_drain_thread();
        pcomm_engine_release();
    }
};
}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::NcclProvider)

static void __attribute__((constructor)) pcomm_nccl_load() {
    nccl::Profiler::instance();
}
static void __attribute__((destructor)) pcomm_nccl_unload() {
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
