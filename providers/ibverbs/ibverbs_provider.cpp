// ibverbs proof provider — the first network provider on pcomm's core. Ported
// from SigmaTrace/Ibtrace: the hook engine (pcomm::hook) drives interposition;
// the callbacks below translate ibv_* handles/args into vendor-neutral pcomm
// records (ib_records.hpp, 100-range) through the Tracer's recorder.
//
// Object-ops hooks (post_send/poll_cq) are installed into ibv_context->ops on
// open_device; the rest are LD_PRELOAD interposers at the bottom of the file.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "pcomm/core/ids.hpp"
#include "pcomm/core/mem_attr.hpp"
#include "pcomm/core/records.hpp"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/tracer.hpp"

#include "ib_event_source.hpp"
#include "ib_records.hpp"
#include "wr_builder.hpp"

#include <mutex>
#include <unordered_map>

namespace ibv {
namespace rr = pcomm::providers::ibverbs::records;
using pcomm::rec::InternDomain;

// Stateless proxy over the C ABI rather than a reference into the engine's
// Recorder — same reason as gpumem: no C++ object layout on the .so boundary.
static pcomm::rec::Recorder<>& rec() {
    static pcomm::rec::Recorder<> proxy;
    return proxy;
}

// Intern a vendor handle (pointer) into a stable Handle for cross-record joins.
static pcomm::core::Handle h(const void* p) {
    return rec().intern(InternDomain::Pointer, reinterpret_cast<std::uint64_t>(p));
}

// Our own port's LID, per QP.
//
// A QP is bound to a port at the INIT transition and nowhere else, so that is
// the one call that can tell us which port — and therefore which LID — this QP
// speaks from. Learned there, remembered for the QP's life, and stamped on
// every transition so a reader never has to join across rows to get it.
//
// This is what makes a QP nameable. qp_num is allocated per HCA, so it collides
// across nodes as a matter of course; with the LID, (src_lid, qp_num) is unique
// because qp_num IS unique within one HCA.
static std::mutex                                       g_lid_mutex;
static std::unordered_map<const ibv_qp*, std::uint32_t> g_qp_lid;

static std::uint32_t local_lid(ibv_qp* qp, const ibv_qp_attr* attr, int mask) {
    if ((mask & IBV_QP_PORT) && qp && qp->context) {
        ibv_port_attr pa{};
        std::uint32_t lid = 0;
        if (ibv_query_port(qp->context, attr->port_num, &pa) == 0)
            lid = pa.lid;
        std::lock_guard<std::mutex> lk(g_lid_mutex);
        g_qp_lid[qp] = lid;
        return lid;
    }
    std::lock_guard<std::mutex> lk(g_lid_mutex);
    auto                        it = g_qp_lid.find(qp);
    return it == g_qp_lid.end() ? 0u : it->second;
}

// Interval [call-start, call-end] from the hook engine's captured rdtsc stamps
// (same clock domain as the recorder + clock anchors), so post_send/poll_cq
// render as slices with real durations instead of instants.
static pcomm::core::Temporal span(std::uint64_t t0, std::uint64_t t1) {
    return pcomm::core::Temporal::interval({0, t0}, {0, t1});
}

// --- the new work-request API ----------------------------------------------
//
// ibv_post_send hands the provider a fully-formed chain, so one hooked call
// sees everything. rdma-core's newer API does not exist as a symbol at all:
// ibv_wr_start/ibv_wr_send/ibv_wr_complete are `static inline` wrappers in
// verbs.h that call function pointers inside `struct ibv_qp_ex`. LD_PRELOAD has
// nothing to bind, and ctx->ops.post_send is never touched — which is why
// ib_send_bw traced end to end produced 202,000 ib_poll_cq records and *zero*
// ib_post_send (MN5 job 43976008). SigmaTrace has the same blind spot.
//
// So the pointers are patched in place, per QP, and the batch is reassembled
// into the same IbPostSend rows the old path emits — same record type, same
// opcode encoding — so a capture never reveals which API the application used
// and every existing query and golden still works.
namespace wr {

using pcomm::providers::ibverbs::WrBuilder;
using pcomm::providers::ibverbs::WrOpcode;

// The provider's own function pointers, saved before we overwrite them.
struct Ops {
    void (*wr_start)(ibv_qp_ex*){};
    int (*wr_complete)(ibv_qp_ex*){};
    void (*wr_abort)(ibv_qp_ex*){};
    void (*wr_send)(ibv_qp_ex*){};
    void (*wr_rdma_write)(ibv_qp_ex*, std::uint32_t, std::uint64_t){};
    void (*wr_rdma_read)(ibv_qp_ex*, std::uint32_t, std::uint64_t){};
    void (*wr_set_sge)(ibv_qp_ex*, std::uint32_t, std::uint64_t, std::uint32_t){};
    void (*wr_set_sge_list)(ibv_qp_ex*, std::size_t, const ibv_sge*){};
    pcomm::core::Handle qp_handle{};
    std::uint32_t       qp_num{};
};

// Written once per QP at creation, read by every wr_* call. A mutex here would
// be on the send path, so the hot path never touches this map directly — see
// the thread-local cache below.
static std::mutex                          g_ops_mutex;
static std::unordered_map<ibv_qp_ex*, Ops> g_ops;

// Per (thread, QP) batch state. The builder must be per-thread because a batch
// is a critical section between wr_start and wr_complete on one thread; the ops
// are copied in so the steady-state send path is a pointer compare and no lock.
struct Slot {
    ibv_qp_ex*    qpx{nullptr};
    Ops           ops{};
    WrBuilder     builder{};
    std::uint64_t t0{0};
};

// The saved originals for a QP, straight from the shared map. Only used when
// the thread-local path cannot serve the call: the application's send must go
// through regardless of whether we manage to record it.
static bool global_ops(ibv_qp_ex* qpx, Ops& out) {
    std::lock_guard lk(g_ops_mutex);
    auto            it = g_ops.find(qpx);
    if (it == g_ops.end())
        return false;
    out = it->second;
    return true;
}

static Slot* slot_for(ibv_qp_ex* qpx) {
    static thread_local std::unordered_map<ibv_qp_ex*, Slot> slots;
    static thread_local Slot* last = nullptr;  // applications post in a loop
    if (last && last->qpx == qpx)
        return last;  // on one QP: this hits ~always

    auto it = slots.find(qpx);
    if (it == slots.end()) {
        Ops ops;
        if (!global_ops(qpx, ops))
            return nullptr;  // not a QP we patched
        it = slots.emplace(qpx, Slot{qpx, ops, {}, 0}).first;
    }
    last = &it->second;
    return last;
}

// INVARIANT for every trampoline below: the application's call reaches the
// provider exactly once, whatever happens to our bookkeeping. A missed record
// costs a row in a capture; a missed wr_start or wr_complete corrupts the
// application's send path. Hence the fallbacks — they cannot trigger for a QP
// we patched, since install() populates g_ops before it swaps any pointer, but
// "cannot happen" is not a reason to leave the failure mode as a hang.

// Forward declarations: the diagnostics below compare the live function
// pointers against ours, and the trampolines are defined in call order.
static void w_start(ibv_qp_ex*);
static void w_send(ibv_qp_ex*);
static int  w_complete(ibv_qp_ex*);
static void w_abort(ibv_qp_ex*);

static void begin(ibv_qp_ex* qpx, WrOpcode op) {
    if (auto* s = slot_for(qpx))
        s->builder.begin(op, qpx->wr_id);
}

static void w_start(ibv_qp_ex* qpx) {
    if (auto* s = slot_for(qpx)) {
        s->builder.start();
        s->t0 = pcomm::helpers::TscClock::now();
        s->ops.wr_start(qpx);
        // mlx5 re-installs its own wr_complete (and may re-install wr_abort)
        // from inside wr_start, choosing an implementation for this batch. That
        // silently unhooks us halfway through: measured on MN5, wr_start and
        // wr_send were ours 1000 times each while wr_complete was the
        // provider's every time, so every batch was built and none was ever
        // recorded — a hook that is installed and still sees nothing.
        //
        // So re-take it after the provider has chosen, and remember what it
        // chose so the batch still reaches the wire.
        // Read each pointer once: comparing and then re-reading would let a
        // concurrent patch of the same QP land in between and store *our own*
        // trampoline as the "original", which recurses forever. The verbs API
        // forbids two threads building a batch on one QP, so this cannot happen
        // today — but the cost of not relying on that is a local variable.
        if (auto* live = qpx->wr_complete; live != &w_complete) {
            s->ops.wr_complete = live;
            qpx->wr_complete = &w_complete;
        }
        if (auto* live = qpx->wr_abort; live != &w_abort) {
            s->ops.wr_abort = live;
            qpx->wr_abort = &w_abort;
        }
        return;
    }
    Ops ops;
    if (global_ops(qpx, ops))
        ops.wr_start(qpx);
}

static void w_send(ibv_qp_ex* qpx) {
    begin(qpx, WrOpcode::Send);
    if (auto* s = slot_for(qpx)) {
        s->ops.wr_send(qpx);
        return;
    }
    Ops ops;
    if (global_ops(qpx, ops))
        ops.wr_send(qpx);
}

static void w_rdma_write(ibv_qp_ex* qpx, std::uint32_t rkey, std::uint64_t addr) {
    begin(qpx, WrOpcode::RdmaWrite);
    if (auto* s = slot_for(qpx)) {
        s->builder.set_rdma(rkey, addr);
        s->ops.wr_rdma_write(qpx, rkey, addr);
        return;
    }
    Ops ops;
    if (global_ops(qpx, ops))
        ops.wr_rdma_write(qpx, rkey, addr);
}

static void w_rdma_read(ibv_qp_ex* qpx, std::uint32_t rkey, std::uint64_t addr) {
    begin(qpx, WrOpcode::RdmaRead);
    if (auto* s = slot_for(qpx)) {
        s->builder.set_rdma(rkey, addr);
        s->ops.wr_rdma_read(qpx, rkey, addr);
        return;
    }
    Ops ops;
    if (global_ops(qpx, ops))
        ops.wr_rdma_read(qpx, rkey, addr);
}

static void w_set_sge(ibv_qp_ex* qpx, std::uint32_t lkey, std::uint64_t addr,
                      std::uint32_t length) {
    if (auto* s = slot_for(qpx)) {
        s->builder.set_sge(length);
        s->ops.wr_set_sge(qpx, lkey, addr, length);
        return;
    }
    Ops ops;
    if (global_ops(qpx, ops))
        ops.wr_set_sge(qpx, lkey, addr, length);
}

static void w_set_sge_list(ibv_qp_ex* qpx, std::size_t num, const ibv_sge* sgl) {
    if (auto* s = slot_for(qpx)) {
        // Only the total and the count reach the record, so a long list needs no
        // allocation on the send path; anything past the cap still posts in full,
        // it is just summed up to the cap. num_sge saturates at 255 regardless,
        // since the record field is a byte.
        std::uint32_t     lens[16];
        const std::size_t n = num < 16 ? num : 16;
        for (std::size_t i = 0; i < n; ++i)
            lens[i] = sgl[i].length;
        s->builder.set_sge_list(n, lens);
        s->ops.wr_set_sge_list(qpx, num, sgl);
        return;
    }
    Ops ops;
    if (global_ops(qpx, ops))
        ops.wr_set_sge_list(qpx, num, sgl);
}

// Where the batch becomes records. The whole batch shares the call interval
// [wr_start, wr_complete-returns], which is the only span that means anything:
// the individual wr_* calls are builder writes, not transmissions.
static int w_complete(ibv_qp_ex* qpx) {
    auto* s = slot_for(qpx);
    if (!s) {
        Ops ops;
        return global_ops(qpx, ops) ? ops.wr_complete(qpx) : 0;
    }
    const int  ret = s->ops.wr_complete(qpx);
    const auto t1 = pcomm::helpers::TscClock::now();
    for (const auto& w : s->builder.complete(ret == 0))
        rec().record(span(s->t0, t1), rr::IbPostSend{{},
                                                     {},
                                                     s->ops.qp_handle,
                                                     w.wr_id,
                                                     w.remote_addr,
                                                     w.rkey,
                                                     w.length,
                                                     s->ops.qp_num,
                                                     w.opcode,
                                                     w.num_sge});
    return ret;
}

static void w_abort(ibv_qp_ex* qpx) {
    if (auto* s = slot_for(qpx)) {
        s->builder.abort();
        s->ops.wr_abort(qpx);
        return;
    }
    Ops ops;
    if (global_ops(qpx, ops))
        ops.wr_abort(qpx);
}

// Patch one QP's work-request pointers. Both handles come from the hooked
// ibv_qp_to_qp_ex call, so nothing here has to decide whether an ibv_qp is
// really the base of an ibv_qp_ex — the application already asked for the
// conversion, and libibverbs already did it.
static void install(ibv_qp* qp, ibv_qp_ex* qpx) {
    if (!qp || !qpx)
        return;

    Ops ops;
    ops.wr_start = qpx->wr_start;
    ops.wr_complete = qpx->wr_complete;
    ops.wr_abort = qpx->wr_abort;
    ops.wr_send = qpx->wr_send;
    ops.wr_rdma_write = qpx->wr_rdma_write;
    ops.wr_rdma_read = qpx->wr_rdma_read;
    ops.wr_set_sge = qpx->wr_set_sge;
    ops.wr_set_sge_list = qpx->wr_set_sge_list;
    ops.qp_handle = h(qp);
    ops.qp_num = qp->qp_num;
    if (!ops.wr_start || !ops.wr_complete)
        return;  // provider does not support it

    {
        std::lock_guard lk(g_ops_mutex);
        if (!g_ops.emplace(qpx, ops).second)
            return;  // already patched
    }

    qpx->wr_start = &w_start;
    qpx->wr_complete = &w_complete;
    if (ops.wr_abort)
        qpx->wr_abort = &w_abort;
    if (ops.wr_send)
        qpx->wr_send = &w_send;
    if (ops.wr_rdma_write)
        qpx->wr_rdma_write = &w_rdma_write;
    if (ops.wr_rdma_read)
        qpx->wr_rdma_read = &w_rdma_read;
    if (ops.wr_set_sge)
        qpx->wr_set_sge = &w_set_sge;
    if (ops.wr_set_sge_list)
        qpx->wr_set_sge_list = &w_set_sge_list;
}

}  // namespace wr

Profiler::Profiler() {
    // The launcher/preload sets PCOMM_* env; bring up sink + recorder once.
    const pcomm::Config cfg = pcomm::Config::from_env();
    // Declare before acquiring: checks our ABI against the engine's and that
    // nobody else has claimed the 100-199 record-id range. pcomm-read merges
    // provider streams assuming those ranges are disjoint; until now nothing
    // verified it.
    if (pcomm_engine_declare_provider("ibverbs", pcomm::core::id_range::ibverbs_lo,
                                      pcomm::core::id_range::ibverbs_hi,
                                      PCOMM_ENGINE_ABI_VERSION) != 0)
        return;  // the engine has already explained why on stderr
    pcomm_engine_acquire(cfg.output_dir.c_str(), cfg.rank, cfg.verbose ? 1 : 0);

    // Install object-ops trampolines into each device context as it opens.
    this->register_post<open_device_tag>([](ibv_context* ctx, ibv_device*, auto...) {
        if (ctx)
            instance().install(ctx);
    });

    // reg_mr_iova2 -> the same memory-region entity. `iova` is the virtual
    // offset the remote side addresses; `addr` is where the memory actually is,
    // and the classic path passes iova == addr, so the record keeps addr and the
    // two paths produce identical rows. A capture cannot tell which entry point
    // registered a region, which is the point — nothing downstream should care.
    // (If an application ever registers with iova != addr, that offset is not in
    // the record. Nothing needs it yet; it would be a version bump, not an edit.)
    this->register_post<reg_mr_iova2_tag>([](ibv_mr* mr, ibv_pd* pd, void* addr, size_t length,
                                             std::uint64_t, unsigned int access, auto...) {
        if (!mr)
            return;
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbMemRegion{{},
                                     {},
                                     h(mr),
                                     h(pd),
                                     {reinterpret_cast<std::uint64_t>(addr), length},
                                     mr->lkey,
                                     mr->rkey,
                                     static_cast<std::uint32_t>(access)});

        // ...and the buffer it registered, attributed to its owning device.
        //
        // Deliberately a second record rather than a `device` column on
        // IbMemRegion. Attribution lives in ONE record type; smearing it across
        // every provider's entity is how you end up with three tables that
        // disagree about the same address. The join is
        // ib_mem_region.base -> mem_object.range.
        //
        // Until now the oldest and largest part of the capture could not say
        // whether a registered buffer was GPU or host memory: case 07 registers
        // NVSHMEM's symmetric heap — device memory moved by GPUDirect RDMA —
        // and nothing on disk said so. The only producer of mem_object was
        // gpumem, which learns it through NVBit, i.e. by instrumenting every
        // global memory instruction for a fact one driver call already knows.
        //
        // MemObject is 520, in the range the engine declares for itself, so a
        // provider that owns 100-199 may emit it without writing outside a
        // declared range. That is what step 2 of the plan bought.
        //
        // Interned on the base address in InternDomain::Pointer, which is the
        // dedup key: two providers that both see this buffer mint the same id,
        // so the duplicate is detectable and mergeable instead of doubling a
        // count — the shape that hooking both ibv_reg_mr and ibv_reg_mr_iova2
        // produced in case 01.
        rec().record(rec().now_instant(), pcomm::core::records::MemObject{
                                              {},
                                              {},
                                              h(addr),
                                              pcomm::core::kNullHandle,
                                              {reinterpret_cast<std::uint64_t>(addr), length},
                                              pcomm::core::device_of(addr)});
    });

    // create_qp -> queue-pair endpoint entity.
    this->register_post<create_qp_tag>([](ibv_qp* qp, ibv_pd* pd, ibv_qp_init_attr* a, auto...) {
        if (!qp)
            return;
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbQueuePair{{},
                                     {},
                                     h(qp),
                                     h(pd),
                                     qp->qp_num,
                                     static_cast<std::uint8_t>(a ? a->qp_type : qp->qp_type)});
    });

    // create_qp_ex -> the extended creation door, reached through the
    // verbs_context function pointer rather than a symbol (see ib_hooks.hpp).
    // This is the door perftest uses, and until it was hooked case 08 recorded
    // a queue pair being modified that nothing had created.
    this->register_post<create_qp_ex_tag>(
        [](ibv_qp* qp, ibv_context*, ibv_qp_init_attr_ex* a, auto...) {
            if (!qp)
                return;
            // Same mask discipline as modify_qp: ibv_qp_init_attr_ex fields are
            // only meaningful when comp_mask says the caller set them, and pd is
            // one of them. qp_type is a base field and always set.
            const bool has_pd = a && (a->comp_mask & IBV_QP_INIT_ATTR_PD);
            rec().record(pcomm::core::Temporal::none(),
                         rr::IbQueuePair{{},
                                         {},
                                         h(qp),
                                         has_pd ? h(a->pd) : pcomm::core::Handle{},
                                         qp->qp_num,
                                         static_cast<std::uint8_t>(a ? a->qp_type : qp->qp_type)});
        });

    // qp_to_qp_ex -> patch this QP's wr_* pointers. Without it the application's
    // sends are invisible: they never reach ctx->ops.post_send, and nothing
    // anywhere fails.
    this->register_post<qp_to_qp_ex_tag>(
        [](ibv_qp_ex* qpx, ibv_qp* qp, auto...) { wr::install(qp, qpx); });

    // modify_qp -> state transition; ->RTR carries the remote endpoint.
    //
    // Read each field only when its mask bit says the caller set it. `mask` is
    // the whole contract of ibv_modify_qp: every field NOT named in it is
    // untouched caller memory, so reading one is reading whatever happened to
    // be on their stack. Keying off the state instead — "we are going to RTR,
    // so the destination must be there" — is wrong for UD, whose RTR needs only
    // IBV_QP_STATE (plus pkey/qkey) and never sets dest_qp_num or ah_attr.
    //
    // Measured on case 02 (NCCL, job 44139911), and the split is total: of 264
    // RTR transitions, the 24 on RC QPs all carried a destination and the 240
    // on UD QPs carried none. We were reading uninitialised bytes 240 times per
    // run and recording whatever came back — here zeroes, which merely looked
    // like "no destination"; there is nothing that makes them zeroes.
    this->register_post<modify_qp_tag>(
        [](int ret, ibv_qp* qp, ibv_qp_attr* attr, int mask, auto...) {
            if (ret != 0 || !qp || !attr)
                return;
            rec().record(rr::IbModifyQp{
                {},
                {},
                h(qp),
                qp->qp_num,
                (mask & IBV_QP_DEST_QPN) ? attr->dest_qp_num : 0u,
                (mask & IBV_QP_AV) ? static_cast<std::uint32_t>(attr->ah_attr.dlid) : 0u,
                local_lid(qp, attr, mask),
                static_cast<std::uint8_t>((mask & IBV_QP_STATE) ? attr->qp_state : 0xFF)});
        });

    // post_send -> one row per work request in the chain (the comm firehose).
    // Records carry the call's [start, end] interval for slice rendering.
    this->register_post<post_send_tag>([](int ret, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr**,
                                          std::uint64_t t0, std::uint64_t t1) {
        if (ret != 0 || !qp)
            return;
        const auto tspan = span(t0, t1);
        for (; wr; wr = wr->next) {
            std::uint32_t length = 0;
            for (int i = 0; i < wr->num_sge; ++i)
                length += wr->sg_list[i].length;
            std::uint64_t raddr = 0;
            std::uint32_t rkey = 0;
            if (wr->opcode == IBV_WR_RDMA_WRITE || wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM ||
                wr->opcode == IBV_WR_RDMA_READ) {
                raddr = wr->wr.rdma.remote_addr;
                rkey = wr->wr.rdma.rkey;
            }
            rec().record(tspan, rr::IbPostSend{{},
                                               {},
                                               h(qp),
                                               wr->wr_id,
                                               raddr,
                                               rkey,
                                               length,
                                               qp->qp_num,
                                               static_cast<std::uint8_t>(wr->opcode),
                                               static_cast<std::uint8_t>(wr->num_sge)});
        }
    });

    // poll_cq -> one row per reaped completion (predicate skips empty polls).
    this->register_post<poll_cq_tag>(
        [](int ret, ibv_cq* cq, int, ibv_wc* wc, std::uint64_t t0, std::uint64_t t1) {
            if (ret <= 0 || !wc)
                return;
            const auto tspan = span(t0, t1);
            for (int i = 0; i < ret; ++i) {
                rec().record(tspan, rr::IbPollCq{{},
                                                 {},
                                                 h(cq),
                                                 wc[i].wr_id,
                                                 wc[i].byte_len,
                                                 wc[i].qp_num,
                                                 static_cast<std::uint8_t>(wc[i].status),
                                                 static_cast<std::uint8_t>(wc[i].opcode)});
            }
        });
}

}  // namespace ibv

// The template is instantiated exactly once, here.
template class ibv::IbverbsEventSource<ibv::Profiler>;

// --- LD_PRELOAD interposers for the preload hooks ----------------------------
// Each forwards through invoke_preload<Tag>, which resolves the real symbol via
// pcomm::smart_dlsym, calls it, and fires the registered pre/post callbacks.

extern "C" ibv_context* ibv_open_device(ibv_device* device) {
    return ibv::Profiler::invoke_preload<ibv::open_device_tag>(device);
}
extern "C" ibv_pd* ibv_alloc_pd(ibv_context* context) {
    return ibv::Profiler::invoke_preload<ibv::alloc_pd_tag>(context);
}
// Interposing this is what makes the new work-request API visible at all: it is
// where a qp_ex first reaches the application, and the wr_* calls that follow
// are static inline wrappers no interposer can reach.
extern "C" ibv_qp_ex* ibv_qp_to_qp_ex(ibv_qp* qp) {
    return ibv::Profiler::invoke_preload<ibv::qp_to_qp_ex_tag>(qp);
}

extern "C" ibv_qp* ibv_create_qp(ibv_pd* pd, ibv_qp_init_attr* attr) {
    return ibv::Profiler::invoke_preload<ibv::create_qp_tag>(pd, attr);
}
extern "C" int ibv_destroy_qp(ibv_qp* qp) {
    return ibv::Profiler::invoke_preload<ibv::destroy_qp_tag>(qp);
}
extern "C" int ibv_modify_qp(ibv_qp* qp, ibv_qp_attr* attr, int mask) {
    return ibv::Profiler::invoke_preload<ibv::modify_qp_tag>(qp, attr, mask);
}
extern "C" int ibv_dereg_mr(ibv_mr* mr) {
    return ibv::Profiler::invoke_preload<ibv::dereg_mr_tag>(mr);
}

// NB there is deliberately no ibv_reg_mr interposer: rdma-core implements it
// over ibv_reg_mr_iova2, so hooking both records every registration twice.
// symbol the ABI actually exports.
// The single registration hook: every path funnels here. See ib_hooks.hpp.
extern "C" ibv_mr* ibv_reg_mr_iova2(ibv_pd* pd, void* addr, size_t length, uint64_t iova,
                                    unsigned int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_mr_iova2_tag>(pd, addr, length, iova, access);
}

// --- provider registration + lifecycle ---------------------------------------

namespace pcomm::providers {

class IbverbsProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"ibverbs", core::id_range::ibverbs_lo, core::id_range::ibverbs_hi};
    }
    bool init(const provider::ProviderConfig&, rec::Recorder<>&) override {
        ibv::Profiler::instance();  // constructs singleton -> starts Tracer + hooks
        return true;
    }
    void shutdown() override {
        pcomm_engine_drain_thread();
        pcomm_engine_release();
    }
};

}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::IbverbsProvider)

// Direct-LD_PRELOAD path (no launcher): bring the singleton up on load and
// flush on unload. NOTE(skeleton): drain runs on the unloading thread only;
// buffers filled by other app threads need the ThreadBufferRegistry (from
// SigmaTrace) for a complete cross-thread flush — a follow-up.
static void __attribute__((constructor)) pcomm_ibverbs_load() {
    ibv::Profiler::instance();
}
static void __attribute__((destructor)) pcomm_ibverbs_unload() {
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
