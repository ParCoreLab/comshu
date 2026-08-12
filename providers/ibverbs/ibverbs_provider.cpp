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
#include "pcomm/core/dmabuf_registry.hpp"
#include "pcomm/core/mem_attr.hpp"
#include "pcomm/core/records.hpp"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/tracer.hpp"

#include "cqe_probe.hpp"
#include "ib_event_source.hpp"
#include "ib_records.hpp"
#include "wr_builder.hpp"

#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>   // dladdr, for the MR caller diagnostic at the bottom
#include <endian.h>  // be64toh — GIDs and GUIDs are big-endian on the wire

namespace ibv {
namespace rr = pcomm::providers::ibverbs::records;
using pcomm::rec::InternDomain;

// How many legacy ibv_reg_mr calls this thread is inside.
//
// rdma-core MAY implement the legacy symbol over ibv_reg_mr_iova2, in which case
// our interposers see one application call twice — that is what case 01 measured
// when both were hooked, and why only iova2 was hooked for a year. But the
// routing is conditional: MN5 job 44368331 found UCX registering through the
// legacy symbol with iova2 never entered, which is where 33 unmatched closes per
// rank were coming from. Neither "it always routes" nor "it never routes" is
// safe to compile in.
//
// So the nesting is observed instead. Only the outermost registration records,
// which makes the legacy hook the recorder when routing happens and the only
// hook when it does not — one row either way.
//
// THREAD-LOCAL, not global: NCCL and UCX register from several proxy threads at
// once, and a shared counter would let one thread's outer call suppress
// another's. This is the same per-thread discipline the flush path already
// needs.
// Instrument a context if it is not already instrumented, and forget it when it
// closes. Defined at the bottom; declared here because the callbacks need them.
//
// `door` names the interposer that is calling, and is recorded for the install
// that actually happens. First-sight install is a floor, not a guarantee: a
// context is instrumented at its first call through a door we hook, so anything
// it did before that is missed. Which door that is bounds the window, and the
// histogram dumped at unload is the only way to know it rather than argue it.
static void ensure_installed(ibv_context* ctx, const char* door);
static void forget_context(const ibv_context* ctx);
// Emits the IbDevice row for a context. Called from ensure_installed, which is
// the one place that runs exactly once per context whatever door it arrives at.
static void record_device(ibv_context* ctx);
// Diagnostics for the unmatched-CQ hunt; see their definitions at the bottom.
static void remember_cq(const ibv_cq* cq);
static void trace_unmatched_destroy_cq(const ibv_cq* cq, void* ret_addr);

static int& reg_mr_depth() noexcept {
    static thread_local int depth = 0;
    return depth;
}

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

// Slots computed by the post_srq_recv PRE-hook, consumed by its post-hook.
// Thread-local: the two halves run on one thread around one call, and this is
// the hottest receive path in the capture, so a per-call allocation is not free.
std::vector<std::uint32_t>& pending_srq_slots() {
    static thread_local std::vector<std::uint32_t> v;
    return v;
}

// (context, qp_num) -> the SRQ that queue pair draws its receives from.
//
// WHY THIS MAP EXISTS, and why the obvious field could not be used instead.
//
// IbRecvCqe has to name the SRQ a completion consumed from, so it can join to
// the slot rows recorded at post time. The CQE appears to carry it — the field
// is literally called `srqn_uidx` — and it does not. Measured on job 44542613:
// mlx5dv reports the true SRQ numbers 30002 and 42471, while the CQEs beside
// them read 3 and 4. That union holds an SRQN only for XRC; for an ordinary
// SRQ it is a user index, and libmlx5 never consults it because the QP already
// knows its own SRQ (`qp->srq`).
//
// So the association is taken from the QP at creation, which is the only place
// it is stated, and looked up at completion by the one identifier a wc carries.
// Keyed by CONTEXT as well as qp_num, because qp_num is allocated per HCA and a
// process with two devices open can hold the same number twice — the same
// collision class that put impossible self-edges in comm_edges when it keyed
// peers on qp_num without the lid.
std::mutex&                                              qp_srq_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::pair<const void*, std::uint32_t>, ibv_srq*>& qp_srq_map() {
    static auto* m = new std::map<std::pair<const void*, std::uint32_t>, ibv_srq*>();  // leaked
    return *m;
}

void note_qp_srq(ibv_qp* qp) {
    if (!qp || !qp->srq || !qp->context)
        return;
    std::lock_guard<std::mutex> lk(qp_srq_mutex());
    qp_srq_map()[{qp->context, qp->qp_num}] = qp->srq;
}

ibv_srq* srq_of_qp(const ibv_context* ctx, std::uint32_t qp_num) {
    std::lock_guard<std::mutex> lk(qp_srq_mutex());
    const auto                  it = qp_srq_map().find({ctx, qp_num});
    return it == qp_srq_map().end() ? nullptr : it->second;
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
    // Watch for dmabuf exports. ibverbs needs fd -> address for
    // ibv_reg_dmabuf_mr exactly as mlx5dv needs it for DEVX umems, and each
    // provider installs its own interception and keeps its own table rather than
    // one asking the other at runtime — the design rule is that a provider may
    // need allocation facts for its own decisions and cannot wait on a peer.
    //
    // Both providers registering "cuGetProcAddress" is only legal because the
    // dlsym registry chains; it used to throw on the second registrant and abort
    // the process.
    pcomm::core::dmabuf::install();

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
    this->register_post<open_device_tag>(
        [](ibv_context* ctx, ibv_device*, auto...) { ensure_installed(ctx, "open_device"); });

    // The mlx5dv open door — the same install-at-open, for the contexts UCX's
    // mlx5 transport opens through it rather than through ibv_open_device. This
    // is the door that closes the entity_close_accounted deficit on cases 02/06/
    // 07: without it those contexts first reached us at ibv_create_cq, blind to
    // every create_cq_ex they had already issued. Its close is still
    // ibv_close_device, which forget_context already handles, so a recycled
    // context pointer is not mistaken for one already patched.
    this->register_post<mlx5dv_open_device_tag>([](ibv_context* ctx, ibv_device*, void*, auto...) {
        ensure_installed(ctx, "mlx5dv_open_device");
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
        // Suppressed when this call is rdma-core's own, made from inside the
        // legacy ibv_reg_mr we are already recording. See kRegMrDepth.
        if (reg_mr_depth() > 0)
            return;
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbMemRegion{{},
                                     {},
                                     h(mr),
                                     h(pd),
                                     {reinterpret_cast<std::uint64_t>(addr), length},
                                     mr->lkey,
                                     mr->rkey,
                                     static_cast<std::uint32_t>(access),
                                     -1,
                                     static_cast<std::uint8_t>(rr::IbRegionOrigin::RegMr)});

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

    // reg_mr -> the legacy registration door, and the same entity. Identical
    // body to iova2 above, because a capture must not be able to tell which
    // entry point registered a region — that is an accident of how the caller
    // was compiled, not a fact about the transfer.
    //
    // Hooked as of 2026-08-07. It was deliberately left unhooked because
    // rdma-core implements it over ibv_reg_mr_iova2, so hooking both recorded
    // every region twice (case 01: one region per rank became two identical
    // rows). That routing turned out to be conditional: MN5 job 44368331 named
    // UCX's mlx5 transport calling ibv_dereg_mr with no hooked registration
    // anywhere, referencing ibv_reg_mr@IBVERBS_1.1, and no libibverbs.so.1
    // appeared as a caller of iova2 in that run at all. Those were the 33
    // unmatched closes entity_close_accounted had been reporting per rank.
    //
    // The double-count is now prevented by observing the nesting rather than by
    // declining to hook: when the routing does happen, the legacy hook records
    // and reg_mr_iova2 above suppresses itself. Which rdma-core build is
    // underneath stops mattering — both behaviours produce exactly one row.
    this->register_post<reg_mr_tag>(
        [](ibv_mr* mr, ibv_pd* pd, void* addr, size_t length, int access, auto...) {
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
                                         static_cast<std::uint32_t>(access),
                                         -1,
                                         static_cast<std::uint8_t>(rr::IbRegionOrigin::RegMr)});
            rec().record(rec().now_instant(), pcomm::core::records::MemObject{
                                                  {},
                                                  {},
                                                  h(addr),
                                                  pcomm::core::kNullHandle,
                                                  {reinterpret_cast<std::uint64_t>(addr), length},
                                                  pcomm::core::device_of(addr)});
        });

    // reg_dm_mr / alloc_null_mr -> the two MEMORYLESS registration doors.
    //
    // `range.base` is 0 here and that is the truth, not a failure to resolve.
    // Device memory lives on the NIC and a null MR has no memory at all, so no
    // host address exists for either — which is precisely why they carry an
    // `origin` and why registration_has_object exempts them by it rather than by
    // base=0, a condition an ordinary broken registration also satisfies.
    //
    // No MemObject for the same reason the dmabuf path withholds one: an object
    // at base 0 is a fiction that range containment then resolves real addresses
    // against.
    //
    // `length` comes from the returned mr, not the argument, so alloc_null_mr —
    // which has no length argument — is described by the same code.
    this->register_post<reg_dm_mr_tag>([](ibv_mr* mr, ibv_pd*       pd, ibv_dm*, std::uint64_t,
                                          std::size_t, unsigned int access, auto...) {
        if (!mr)
            return;
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbMemRegion{{},
                                     {},
                                     h(mr),
                                     h(pd),
                                     {0, mr->length},
                                     mr->lkey,
                                     mr->rkey,
                                     static_cast<std::uint32_t>(access),
                                     -1,
                                     static_cast<std::uint8_t>(rr::IbRegionOrigin::DeviceMemory)});
    });
    this->register_post<alloc_null_mr_tag>([](ibv_mr* mr, ibv_pd* pd, auto...) {
        if (!mr)
            return;
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbMemRegion{{},
                                     {},
                                     h(mr),
                                     h(pd),
                                     {0, mr->length},
                                     mr->lkey,
                                     mr->rkey,
                                     0,
                                     -1,
                                     static_cast<std::uint8_t>(rr::IbRegionOrigin::NullMr)});
    });

    // close_device -> the context is gone. Forget it, so the next context to be
    // handed the same address by the allocator is instrumented rather than
    // mistaken for this one. See forget_context.
    // ...and the device entity's lifetime ends with it. Without this every
    // ib_device row would be an entity opened and never closed, which is a case
    // suite/asserts/entity_close_accounted.sql reports by design.
    this->register_post<close_device_tag>([](int ret, ibv_context* ctx, auto...) {
        if (ret != 0)
            return;
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{
                                              {}, {}, h(ctx), rr::IbDevice::pcomm_type_id});
        forget_context(ctx);
    });

    // create_cq / create_cq_ex -> the completion-queue entity.
    //
    // Until this landed, IbPollCq.cq was an interned pointer joining to NOTHING:
    // the oldest event table in the capture named a subject no record created.
    // entity_close_accounted could not see it, because that assertion checks
    // closes against opens and an event's subject is neither.
    //
    // Both doors record the same row. The extended one is not a variant to be
    // told apart downstream — the same reasoning that gives IbMemRegion a single
    // shape for reg_mr and reg_mr_iova2 — so nothing in the capture reveals
    // which entry point the caller happened to compile against.
    //
    // `cqe` comes from the RETURNED cq, not from the argument: ibv_create_cq
    // takes a minimum and the device grants at least that many, so the argument
    // is a request and cq->cqe is the fact.
    this->register_post<create_cq_tag>([](ibv_cq* cq, ibv_context* ctx, int, void*,
                                          ibv_comp_channel* channel, int comp_vector, auto...) {
        if (!cq)
            return;
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbCompletionQueue{{},
                                           {},
                                           h(cq),
                                           h(ctx),
                                           channel ? h(channel) : pcomm::core::Handle{},
                                           static_cast<std::uint32_t>(cq->cqe),
                                           static_cast<std::uint32_t>(comp_vector),
                                           static_cast<std::uint8_t>(rr::IbCqOrigin::CreateCq)});
        remember_cq(cq);
    });

    // The extended door. ibv_cq_ex starts with ibv_cq's exact layout (verbs.h
    // makes ibv_cq_ex_to_cq a plain cast), so the returned pointer interns to
    // the same Handle a poll on this CQ will produce, and the fields below are
    // read through that cast rather than duplicated.
    //
    // comp_vector is a plain field of ibv_cq_init_attr_ex and needs no comp_mask
    // check: the mask governs the OPTIONAL tail (flags, parent_domain), not the
    // head. That distinction is the modify_qp lesson — reading a field the
    // caller never set is reading their stack — so it is stated rather than
    // assumed.
    this->register_post<create_cq_ex_tag>(
        [](ibv_cq_ex* cqx, ibv_context* ctx, ibv_cq_init_attr_ex* a, auto...) {
            if (!cqx)
                return;
            ibv_cq* cq = ibv_cq_ex_to_cq(cqx);
            rec().record(
                pcomm::core::Temporal::none(),
                rr::IbCompletionQueue{{},
                                      {},
                                      h(cq),
                                      h(ctx),
                                      (a && a->channel) ? h(a->channel) : pcomm::core::Handle{},
                                      static_cast<std::uint32_t>(cq->cqe),
                                      a ? a->comp_vector : 0u,
                                      static_cast<std::uint8_t>(rr::IbCqOrigin::CreateCqEx)});
            remember_cq(cq);
        });

    // mlx5dv_create_cq -> the same entity through libmlx5's own door.
    //
    // Recorded as an ib_completion_queue rather than as a provider-local record
    // because that is what it is: the returned ibv_cq_ex* is the pointer
    // ibv_destroy_cq will be handed and the pointer ib_poll_cq already names, so
    // a second table would only force every consumer to union two. The origin
    // column is what keeps the doors distinguishable, which is the whole reason
    // it exists.
    //
    // Fields read through the same ibv_cq_ex_to_cq cast create_cq_ex uses, and
    // for the same reason: cq->cqe is the depth the DEVICE granted. The
    // mlx5dv_cq_init_attr* third argument is never dereferenced (see ib_hooks.hpp).
    this->register_post<mlx5dv_create_cq_tag>(
        [](ibv_cq_ex* cqx, ibv_context* ctx, ibv_cq_init_attr_ex* a, void*, auto...) {
            if (!cqx)
                return;
            ibv_cq* cq = ibv_cq_ex_to_cq(cqx);
            rec().record(
                pcomm::core::Temporal::none(),
                rr::IbCompletionQueue{{},
                                      {},
                                      h(cq),
                                      h(ctx),
                                      (a && a->channel) ? h(a->channel) : pcomm::core::Handle{},
                                      static_cast<std::uint32_t>(cq->cqe),
                                      a ? a->comp_vector : 0u,
                                      static_cast<std::uint8_t>(rr::IbCqOrigin::Mlx5dvCreateCq)});
            remember_cq(cq);
        });

    // destroy_cq -> the CQ's lifetime ended. Same success-only discipline as
    // dereg_mr and destroy_qp: a destroy that failed closed nothing, and
    // recording it would make a live CQ look freed.
    this->register_post<destroy_cq_tag>([](int ret, ibv_cq* cq, auto...) {
        if (ret != 0 || !cq)
            return;
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{
                                              {}, {}, h(cq), rr::IbCompletionQueue::pcomm_type_id});
    });

    // create_ah -> the UD path's addressing object, and the only place in the
    // API where a UD destination is named.
    //
    // An RC QP learns its peer at modify_qp->RTR and IbModifyQp records it. UD
    // has no such transition — its RTR sets only state, pkey and qkey — so
    // without this row a UD workload contributes nothing to comm_edges at all,
    // however much traffic it moves.
    //
    // The GRH is read only when is_global says it was filled in. Same discipline
    // as modify_qp's mask: a field the caller did not set is their stack, and
    // this provider has already paid once for reading 240 of those per run.
    this->register_post<create_ah_tag>([](ibv_ah* ah, ibv_pd* pd, ibv_ah_attr* a, auto...) {
        if (!ah || !a)
            return;
        std::uint64_t prefix = 0, iface = 0;
        if (a->is_global) {
            std::memcpy(&prefix, &a->grh.dgid.raw[0], 8);
            std::memcpy(&iface, &a->grh.dgid.raw[8], 8);
            prefix = be64toh(prefix);
            iface = be64toh(iface);
        }
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbAddressHandle{{},
                                         {},
                                         h(ah),
                                         h(pd),
                                         prefix,
                                         iface,
                                         a->is_global ? a->grh.flow_label : 0u,
                                         static_cast<std::uint32_t>(a->dlid),
                                         a->sl,
                                         a->port_num,
                                         a->is_global,
                                         a->is_global ? a->grh.sgid_index : std::uint8_t{0},
                                         a->is_global ? a->grh.hop_limit : std::uint8_t{0},
                                         a->is_global ? a->grh.traffic_class : std::uint8_t{0}});
    });

    this->register_post<destroy_ah_tag>([](int ret, ibv_ah* ah, auto...) {
        if (ret != 0 || !ah)
            return;
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{
                                              {}, {}, h(ah), rr::IbAddressHandle::pcomm_type_id});
    });

    // create_qp -> queue-pair endpoint entity.
    this->register_post<create_qp_tag>([](ibv_qp* qp, ibv_pd* pd, ibv_qp_init_attr* a, auto...) {
        if (!qp)
            return;
        note_qp_srq(qp);
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbQueuePair{{},
                                     {},
                                     h(qp),
                                     h(pd),
                                     qp->qp_num,
                                     static_cast<std::uint8_t>(a ? a->qp_type : qp->qp_type)});
        // The close lives in the engine (ibv_destroy_qp tears down ibverbs' and
        // mlx5dv's queue pairs alike, so no provider may own it). Tell it this
        // open was recorded, so it closes this handle as an ib_queue_pair rather
        // than falling back to the per-run guess.
        pcomm_engine_note_qp(qp, rr::IbQueuePair::pcomm_type_id);
    });

    // create_qp_ex -> the extended creation door, reached through the
    // verbs_context function pointer rather than a symbol (see ib_hooks.hpp).
    // This is the door perftest uses, and until it was hooked case 08 recorded
    // a queue pair being modified that nothing had created.
    this->register_post<create_qp_ex_tag>(
        [](ibv_qp* qp, ibv_context*, ibv_qp_init_attr_ex* a, auto...) {
            if (!qp)
                return;
            note_qp_srq(qp);
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
            pcomm_engine_note_qp(qp, rr::IbQueuePair::pcomm_type_id);
        });

    // reg_dmabuf_mr -> a region the NIC was given by descriptor.
    //
    // The address is recovered in the order most-trustworthy first:
    //
    //   1. the fd -> range table, populated by our own interception of
    //      cuMemGetHandleForAddressRange (core/dmabuf_registry.hpp). This is the
    //      only source that also yields the owning DEVICE.
    //   2. `iova`, when the caller supplied one. NCCL passes the real address
    //      there on its net_ib path (reg.cc:35) and 0 on its GDAKI path
    //      (gin_host_gdaki.cc:131), so this is right when it is nonzero and
    //      absent when it is not.
    //
    // If neither yields an address the region is still recorded — it exists, and
    // an unrecorded region is what made these buffers invisible in the first
    // place — with base 0 and no MemObject. Synthesising an object at base 0
    // would put a fiction in the capture that range containment then resolves
    // real addresses against, which is the same thing mlx5dv refuses for dmabuf
    // umems.
    this->register_post<reg_dmabuf_mr_tag>([](ibv_mr* mr, ibv_pd* pd, std::uint64_t offset,
                                              std::size_t length, std::uint64_t iova, int fd,
                                              int access, auto...) {
        if (!mr)
            return;
        pcomm::core::dmabuf::Range  r{};
        const bool                  resolved = pcomm::core::dmabuf::lookup(fd, &r);
        const std::uint64_t         base = resolved ? r.base + offset : iova;
        const pcomm::core::DeviceId device =
            resolved ? r.device
                     : (base ? pcomm::core::device_of(reinterpret_cast<const void*>(base))
                             : pcomm::core::kUnknownDevice);

        rec().record(pcomm::core::Temporal::none(),
                     rr::IbMemRegion{{},
                                     {},
                                     h(mr),
                                     h(pd),
                                     {base, length},
                                     mr->lkey,
                                     mr->rkey,
                                     static_cast<std::uint32_t>(access),
                                     fd,
                                     static_cast<std::uint8_t>(rr::IbRegionOrigin::Dmabuf)});

        if (base)
            rec().record(rec().now_instant(),
                         pcomm::core::records::MemObject{{},
                                                         {},
                                                         h(reinterpret_cast<const void*>(base)),
                                                         pcomm::core::kNullHandle,
                                                         {base, length},
                                                         device});
    });

    // dereg_mr / destroy_qp -> the entity's lifetime ended.
    //
    // Both hooks existed and neither emitted anything, so a capture could count
    // registration CALLS but never live regions — which is why case 06's region
    // count tracks NVSHMEM's allocator churn and had to retreat to per-rank
    // symmetry, and why range containment in pcomm-read is sound only while no
    // address is reused.
    //
    // Only on success: a dereg that failed closed nothing, and recording it
    // would make a live region look freed — worse than the missing row, because
    // the error is silent and downstream.
    //
    // The subject is the same interned pointer the open record carries
    // (IbMemRegion h(mr), IbQueuePair h(qp)), so the join is exact.
    this->register_post<dereg_mr_tag>([](int ret, ibv_mr* mr, auto...) {
        if (ret != 0 || !mr)
            return;
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{
                                              {}, {}, h(mr), rr::IbMemRegion::pcomm_type_id});
    });
    // ibv_destroy_qp's close is emitted by the engine, not here — see ib_hooks.hpp
    // and src/engine.cpp. A QP created through mlx5dv_create_qp is destroyed
    // through the same door, and only the engine is loaded in every selection.

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

    // post_recv -> one row per posted receive. The other half of the wire.
    //
    // Same chain walk as post_send above, because ibv_post_recv takes the same
    // shape of linked list. What differs is what is kept: the FIRST sge's addr
    // and lkey, which is the field SigmaTrace's handler does not have and the
    // reason this record answers "which memory receives this" when its does not.
    //
    // `length` is the total over the sg_list — the capacity being offered, not
    // the bytes that will arrive. How many actually land is byte_len on the
    // completion, and the two differing is a legal short receive rather than a
    // discrepancy.
    //
    // Success only, and for a sharper reason than post_send's: a failed
    // ibv_post_recv posts NO buffer, so recording one would invent a receive
    // that the hardware will never complete — and the assertion this record
    // exists to support (every recv completion joins to a posted receive) would
    // then be comparing against a receive that never happened.
    this->register_post<post_recv_tag>([](int ret, ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr**,
                                          std::uint64_t t0, std::uint64_t t1) {
        if (ret != 0 || !qp)
            return;
        const auto tspan = span(t0, t1);
        for (; wr; wr = wr->next) {
            // Establish that the list is readable BEFORE reading it. num_sge is
            // a count, not a promise that sg_list is non-null, and this runs on
            // every posted receive in the capture — a chain walk that trusts the
            // count and then guards the pointer has the check in the wrong order
            // and dereferences null on the way past it.
            const bool    has_sge = wr->num_sge > 0 && wr->sg_list != nullptr;
            std::uint32_t length = 0;
            if (has_sge)
                for (int i = 0; i < wr->num_sge; ++i)
                    length += wr->sg_list[i].length;
            rec().record(tspan,
                         rr::IbPostRecv{{},
                                        {},
                                        h(qp),
                                        wr->wr_id,
                                        has_sge ? wr->sg_list[0].addr : 0u,
                                        has_sge ? wr->sg_list[0].lkey : 0u,
                                        length,
                                        has_sge ? wr->sg_list[0].length : 0u,
                                        qp->qp_num,
                                        // A QP receive queue is not a shared
                                        // ring and needs no slot: (qp_num,
                                        // wr_id) already identifies it exactly.
                                        rr::kSlotUnknown,
                                        static_cast<std::uint8_t>(wr->num_sge),
                                        static_cast<std::uint8_t>(rr::IbRecvOrigin::QueuePair)});
        }
    });

    // post_srq_recv -> the same IbPostRecv row, discriminated by origin.
    //
    // Deliberately NOT a second record type. A reader asking "what receives were
    // posted" should not have to know which API posted them, exactly as it does
    // not have to know whether a region came through reg_mr or reg_mr_iova2.
    // `subject` is the SRQ here rather than a QP, and `qp_num` is 0 because an
    // SRQ genuinely has none — the receive is not bound to an endpoint until the
    // hardware picks one, which is the whole point of a shared receive queue.
    //
    // That is also the limit of what this row can claim: an SRQ receive cannot
    // be joined to a completion by (qp, wr_id) the way a QP receive can, because
    // the QP is only decided when the message lands. The recv-completion
    // assertion scopes itself to origin='qp' for that reason, and says so.
    // THE SLOT IS READ BEFORE THE CALL AND RECORDED AFTER IT, which is why this
    // door has both halves of a hook where every other one here has a post.
    //
    // mlx5dv_srq.head names where the NEXT posted WQE will go, so once
    // ibv_post_srq_recv has run it reports the following post's slots and not
    // this one's. But the row itself must be written afterwards: a post that
    // FAILS consumed nothing, and recording from the pre-hook would put buffers
    // in the capture that the queue never accepted. So the pre-hook stashes and
    // the post-hook commits — thread-local because the two halves are the same
    // thread by construction, and a per-call vector would allocate on the
    // hottest receive path in the capture.
    this->register_pre<post_srq_recv_tag>(
        [](ibv_srq* srq, ibv_recv_wr* wr, ibv_recv_wr**, std::uint64_t) {
            auto& slots = pending_srq_slots();
            slots.clear();
            if (!srq || !wr)
                return;
            for (unsigned n = 0; wr; wr = wr->next, ++n) {
                const auto s = cqeprobe::srq_slot(srq, n);
                slots.push_back(s.valid ? s.slot : rr::kSlotUnknown);
            }
        });

    this->register_post<post_srq_recv_tag>([](int ret, ibv_srq* srq, ibv_recv_wr* wr, ibv_recv_wr**,
                                              std::uint64_t t0, std::uint64_t t1) {
        auto& slots = pending_srq_slots();
        if (ret != 0 || !srq) {
            slots.clear();
            return;
        }
        const auto tspan = span(t0, t1);
        std::size_t n = 0;
        for (; wr; wr = wr->next, ++n) {
            // Guard before read, as in post_recv above.
            const bool    has_sge = wr->num_sge > 0 && wr->sg_list != nullptr;
            std::uint32_t length = 0;
            if (has_sge)
                for (int i = 0; i < wr->num_sge; ++i)
                    length += wr->sg_list[i].length;
            rec().record(tspan, rr::IbPostRecv{
                                    {},
                                    {},
                                    h(srq),
                                    wr->wr_id,
                                    has_sge ? wr->sg_list[0].addr : 0u,
                                    has_sge ? wr->sg_list[0].lkey : 0u,
                                    length,
                                    has_sge ? wr->sg_list[0].length : 0u,
                                    0u,
                                    n < slots.size() ? slots[n] : rr::kSlotUnknown,
                                    static_cast<std::uint8_t>(wr->num_sge),
                                    static_cast<std::uint8_t>(rr::IbRecvOrigin::SharedRecvQueue)});
        }
        slots.clear();
    });

    // create_srq / create_srq_ex -> the shared-receive-queue entity, so no
    // IbPostRecv row of origin `srq` names an entity nothing created.
    //
    // The depths come from ibv_query_srq rather than from the caller's request,
    // for the same reason IbCompletionQueue.cqe comes from cq->cqe: the argument
    // is a minimum and the device's answer is the fact. Falling back to the
    // request on failure keeps the row rather than the precision — an SRQ that
    // exists and is described approximately is worth more than an entity the
    // capture cannot see at all, which is the defect this whole stage removes.
    //
    // Calling back into libibverbs from a post callback is established here:
    // local_lid() does exactly this with ibv_query_port on the modify_qp path.
    // ibv_query_srq is not a door we interpose, so there is no recursion.
    this->register_post<create_srq_tag>(
        [](ibv_srq* srq, ibv_pd* pd, ibv_srq_init_attr* a, auto...) {
            if (!srq)
                return;
            ibv_srq_attr        q{};
            const bool          ok = ibv_query_srq(srq, &q) == 0;
            const ibv_srq_attr& s = (ok || !a) ? q : a->attr;
            rec().record(
                pcomm::core::Temporal::none(),
                rr::IbSharedRecvQueue{{}, {}, h(srq), h(pd), s.max_wr, s.max_sge, s.srq_limit});
        });

    // The extended door. `pd` is comp_mask-gated here — the same discipline
    // create_qp_ex needs, and for the same reason modify_qp reads nothing the
    // mask does not name: a field the caller never set is their stack, not data.
    this->register_post<create_srq_ex_tag>(
        [](ibv_srq* srq, ibv_context*, ibv_srq_init_attr_ex* a, auto...) {
            if (!srq)
                return;
            ibv_srq_attr        q{};
            const bool          ok = ibv_query_srq(srq, &q) == 0;
            const ibv_srq_attr& s = (ok || !a) ? q : a->attr;
            const bool          has_pd = a && (a->comp_mask & IBV_SRQ_INIT_ATTR_PD);
            rec().record(pcomm::core::Temporal::none(),
                         rr::IbSharedRecvQueue{{},
                                               {},
                                               h(srq),
                                               has_pd ? h(a->pd) : pcomm::core::Handle{},
                                               s.max_wr,
                                               s.max_sge,
                                               s.srq_limit});
        });

    // destroy_srq -> the entity's lifetime ended. Success only, like every other
    // close on this path.
    this->register_post<destroy_srq_tag>([](int ret, ibv_srq* srq, auto...) {
        if (ret != 0 || !srq)
            return;
        rec().record(rec().now_instant(),
                     pcomm::core::records::EntityClose{
                         {}, {}, h(srq), rr::IbSharedRecvQueue::pcomm_type_id});
    });

    // req_notify_cq -> the CQ was armed. The counterpart to poll_cq, and the
    // only row that distinguishes an application sleeping on a completion
    // channel from one spinning on an empty CQ.
    //
    // No predicate: unlike poll_cq there is no firehose to filter, because a CQ
    // is armed once per batch rather than once per completion.
    this->register_post<req_notify_cq_tag>([](int ret, ibv_cq* cq, int solicited_only,
                                              std::uint64_t t0, std::uint64_t t1) {
        if (ret != 0 || !cq)
            return;
        rec().record(
            span(t0, t1),
            rr::IbReqNotifyCq{{}, {}, h(cq), static_cast<std::uint8_t>(solicited_only ? 1 : 0)});
    });

    // poll_cq -> one row per reaped completion (predicate skips empty polls).
    this->register_post<poll_cq_tag>(
        [](int ret, ibv_cq* cq, int, ibv_wc* wc, std::uint64_t t0, std::uint64_t t1) {
            // DIAGNOSTIC, gated on PCOMM_CQE_PROBE=1 and off by default: read
            // the raw mlx5 CQE behind each of these completions and check it
            // against them. See cqe_probe.hpp for the question it settles.
            cqeprobe::on_poll(cq, ret, wc);
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

                // The hardware's own identification of the WQE this receive
                // consumed, when the CQE can be read AND agrees with the ibv_wc
                // above. See ib_records.hpp for why this is a separate row
                // rather than four more columns on the record just written.
                //
                // Receives only: a requester completion has no receive WQE to
                // name, and >98% of completions are requester-side, so this
                // branch is the difference between a free feature and a tax on
                // every poll.
                if (wc[i].status != IBV_WC_SUCCESS || !(wc[i].opcode & IBV_WC_RECV))
                    continue;
                const auto id = cqeprobe::read_recv_cqe(cq, ret, i, wc[i]);
                if (!id.valid)
                    continue;
                // The SRQ comes from the QP, not from the CQE — see the comment
                // on qp_srq_map above for the measurement that forced that.
                ibv_srq* srq = cq->context ? srq_of_qp(cq->context, wc[i].qp_num) : nullptr;
                rec().record(tspan, rr::IbRecvCqe{{},
                                                  {},
                                                  h(cq),
                                                  wc[i].wr_id,
                                                  srq ? h(srq) : pcomm::core::Handle{},
                                                  id.wqe_counter,
                                                  wc[i].qp_num,
                                                  wc[i].byte_len});
            }
        });
}

}  // namespace ibv

// The template is instantiated exactly once, here.
template class ibv::IbverbsEventSource<ibv::Profiler>;

// --- LD_PRELOAD interposers for the preload hooks ----------------------------
// Each forwards through invoke_preload<Tag>, which resolves the real symbol via
// pcomm::smart_dlsym, calls it, and fires the registered pre/post callbacks.

extern "C" int ibv_close_device(ibv_context* context) {
    return ibv::Profiler::invoke_preload<ibv::close_device_tag>(context);
}
extern "C" ibv_context* ibv_open_device(ibv_device* device) {
    return ibv::Profiler::invoke_preload<ibv::open_device_tag>(device);
}
// libmlx5's context-open door. A libmlx5 symbol interposed by the ibverbs
// provider, deliberately, exactly like mlx5dv_create_cq below — the entity these
// contexts create and destroy is accounted from here, so the open must be too.
// The mlx5dv_context_attr* second argument is void* here and ABI-identical; the
// C symbol has no mangling to disagree about, so libmlx5 and its callers cannot
// tell the difference.
extern "C" ibv_context* mlx5dv_open_device(ibv_device* device, void* mlx5_attr) {
    return ibv::Profiler::invoke_preload<ibv::mlx5dv_open_device_tag>(device, mlx5_attr);
}
// Two jobs, and it had only the first for a long time. As COVERAGE: a CQ is
// created from a context, not from a PD, so this is one of the doors through
// which an object-op (poll_cq) could reach a context we have not installed into
// yet. As a RECORD: it emits ib_completion_queue, the entity IbPollCq.cq had
// been referencing since ib_poll_cq existed without anything creating it.
extern "C" ibv_cq* ibv_create_cq(ibv_context* context, int cqe, void* cq_context,
                                 ibv_comp_channel* channel, int comp_vector) {
    ibv::ensure_installed(context, "create_cq");
    return ibv::Profiler::invoke_preload<ibv::create_cq_tag>(context, cqe, cq_context, channel,
                                                             comp_vector);
}
// libmlx5's completion-queue door. A libmlx5 symbol interposed by the ibverbs
// provider, which is deliberate: see ib_hooks.hpp. ensure_installed for exactly
// the reason create_cq has it — a CQ hangs off a context, so this is a door
// through which a context carrying only completion queues can first reach us,
// and it is the door UCX's mlx5 transport actually uses.
//
// The third parameter is mlx5dv_cq_init_attr* in mlx5dv.h and void* here: the
// declarations are ABI-identical and the C symbol has no mangling to disagree
// about, so nothing in libmlx5 or its callers can tell the difference.
extern "C" ibv_cq_ex* mlx5dv_create_cq(ibv_context* context, ibv_cq_init_attr_ex* cq_attr,
                                       void* mlx5_cq_attr) {
    ibv::ensure_installed(context, "mlx5dv_create_cq");
    return ibv::Profiler::invoke_preload<ibv::mlx5dv_create_cq_tag>(context, cq_attr, mlx5_cq_attr);
}
// The CQ's other end. ensure_installed here for the same reason create_cq has
// it: a CQ hangs off a context rather than a PD, so this is one of the two doors
// through which a context carrying only completion queues can first reach us.
extern "C" int ibv_destroy_cq(ibv_cq* cq) {
    if (cq)
        ibv::ensure_installed(cq->context, "destroy_cq");
    ibv::trace_unmatched_destroy_cq(cq, __builtin_return_address(0));
    return ibv::Profiler::invoke_preload<ibv::destroy_cq_tag>(cq);
}
// The classic SRQ doors. ibv_create_srq takes a PD, so unlike create_cq it can
// never be the first call on a context — the PD had to exist. It still calls
// ensure_installed because doing so is free and the rule here is that every
// interposer installs, rather than each one arguing about whether it needs to.
extern "C" ibv_srq* ibv_create_srq(ibv_pd* pd, ibv_srq_init_attr* attr) {
    if (pd)
        ibv::ensure_installed(pd->context, "create_srq");
    return ibv::Profiler::invoke_preload<ibv::create_srq_tag>(pd, attr);
}
extern "C" int ibv_destroy_srq(ibv_srq* srq) {
    if (srq)
        ibv::ensure_installed(srq->context, "destroy_srq");
    return ibv::Profiler::invoke_preload<ibv::destroy_srq_tag>(srq);
}
extern "C" ibv_pd* ibv_alloc_pd(ibv_context* context) {
    ibv::ensure_installed(context, "alloc_pd");
    return ibv::Profiler::invoke_preload<ibv::alloc_pd_tag>(context);
}
// Interposing this is what makes the new work-request API visible at all: it is
// where a qp_ex first reaches the application, and the wr_* calls that follow
// are static inline wrappers no interposer can reach.
extern "C" ibv_qp_ex* ibv_qp_to_qp_ex(ibv_qp* qp) {
    if (qp)
        ibv::ensure_installed(qp->context, "qp_to_qp_ex");
    return ibv::Profiler::invoke_preload<ibv::qp_to_qp_ex_tag>(qp);
}

extern "C" ibv_qp* ibv_create_qp(ibv_pd* pd, ibv_qp_init_attr* attr) {
    if (pd)
        ibv::ensure_installed(pd->context, "create_qp");
    return ibv::Profiler::invoke_preload<ibv::create_qp_tag>(pd, attr);
}
// ibv_destroy_qp is defined by the engine (src/engine.cpp), which owns the QP
// close for every provider selection. It is not an install door here: a QP
// reaching destroy was created through ibv_create_qp or mlx5dv_create_qp, both of
// which install their context, so nothing is lost by not installing at destroy.
extern "C" int ibv_modify_qp(ibv_qp* qp, ibv_qp_attr* attr, int mask) {
    if (qp)
        ibv::ensure_installed(qp->context, "modify_qp");
    return ibv::Profiler::invoke_preload<ibv::modify_qp_tag>(qp, attr, mask);
}
// The UD addressing doors.
extern "C" ibv_ah* ibv_create_ah(ibv_pd* pd, ibv_ah_attr* attr) {
    if (pd)
        ibv::ensure_installed(pd->context, "create_ah");
    return ibv::Profiler::invoke_preload<ibv::create_ah_tag>(pd, attr);
}
extern "C" int ibv_destroy_ah(ibv_ah* ah) {
    if (ah)
        ibv::ensure_installed(ah->context, "destroy_ah");
    return ibv::Profiler::invoke_preload<ibv::destroy_ah_tag>(ah);
}
// Install door only — the ib_device row is emitted from ensure_installed, not
// from here. See the query_device hook in ib_hooks.hpp.
extern "C" int ibv_query_device(ibv_context* context, ibv_device_attr* attr) {
    ibv::ensure_installed(context, "query_device");
    return ibv::Profiler::invoke_preload<ibv::query_device_tag>(context, attr);
}
// DIAGNOSTIC, gated on PCOMM_TRACE_MR_CALLERS=1 — which LIBRARY calls each
// registration door?
//
// entity_close_accounted reports 33 closes per rank naming regions nothing
// opened, all opener=ib_mem_region. Both registration hooks record their region
// unconditionally on success, so an unmatched close means the registration never
// entered this file at all — while the matching ibv_dereg_mr did.
//
// The search for the missing door assumed NCCL, on the strength of its source
// resolving ibv_* through dlvsym. Job 44368260 killed that: dlvsym is entered on
// all 8 ranks and never once with an ibv_* name, and neither is dlsym. So
// nothing in that process resolves ibverbs dynamically, and the caller is
// somebody nobody has named yet — OpenMPI/UCX being the obvious unexamined
// occupant of a run everyone has been reading as pure NCCL.
//
// Comparing the caller set of dereg against the caller set of the two reg doors
// answers that directly: a library that appears only under dereg registers by a
// path we do not hook. This reports the CALLER, which is what neither the symbol
// tools nor the resolution trace could give.
//
// Must be called from the interposer itself: __builtin_return_address(0) means
// the caller of the frame it appears in, so in a helper it names this file.
static bool trace_mr_callers_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_TRACE_MR_CALLERS");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

// `length` is part of the dedup key, and that is not cosmetic. The per-(door,
// library) table cannot say WHICH library registered a particular region, so
// when entity_close_accounted reports "the 40-byte and 8 MiB regions on each
// node leader are never closed" it cannot name their owner — and `reg_mr` has
// two callers. Keying on size splits them, because the leak was identified BY
// size in the first place. len=-1 means the door does not carry one.
//
// Bounded, because at scale this could otherwise print a line per distinct
// allocation size on every rank: 256 distinct rows per process, then silence
// with one final line saying so. A diagnostic that floods the job log is one
// that gets turned off, and its last line is the one you needed.
static void trace_mr_caller(const char* door, void* ret_addr, long length = -1) {
    if (!trace_mr_callers_enabled())
        return;
    Dl_info           info{};
    const char*       who = (dladdr(ret_addr, &info) && info.dli_fname) ? info.dli_fname : "?";
    static std::mutex m;
    std::lock_guard<std::mutex>  lk(m);
    static std::set<std::string> seen;
    static bool                  capped = false;
    if (capped)
        return;
    if (!seen.insert(std::string(door) + "|" + who + "|" + std::to_string(length)).second)
        return;
    if (seen.size() > 256) {
        capped = true;
        std::fprintf(stderr, "[pcomm-mrcaller] (capped at 256 distinct door|lib|len rows)\n");
        std::fflush(stderr);
        return;
    }
    std::fprintf(stderr, "[pcomm-mrcaller] %-20s <- %s len=%ld\n", door, who, length);
    std::fflush(stderr);
}

// The same diagnostic, narrowed from "who calls this door" to "who closes a
// region we never saw opened" — which is the actual question, and one the
// per-door caller table above could not answer.
//
// That table showed libuct_ib_mlx5.so.0 calling only dereg_mr, and I read it as
// naming the missing door. It does not: that library also references
// ibv_reg_mr_iova2 and calls neither reg door, so the MRs it deregisters were
// very likely registered by libuct_ib.so.0, which does register through a hooked
// door. A per-library summary cannot distinguish "registered elsewhere" from
// "registered through a door we do not hook"; only following the individual mr
// can.
//
// So every registration door records its ibv_mr* here, and dereg reports the
// ones that are absent, with the caller AND the buffer. Size matters: a 1 GB
// unmatched region is a symmetric heap and a 4 KB one is a control buffer, and
// they imply different missing doors.
//
// Deliberately not the capture's own bookkeeping — this set is diagnostic-only
// and never consulted by a record path, so it cannot change what is captured.
static std::mutex             g_known_mr_mutex;
static std::set<const void*>* g_known_mrs = nullptr;  // leaked: outlives atexit

// Which ibv_contexts we installed object-ops into, i.e. those that came through
// ibv_open_device.
//
// Object-ops hooks are per-context: install() patches slots inside one
// ibv_context, so a context opened by a door we do not hook is invisible to
// EVERY object-ops hook at once — post_send and poll_cq included, which would be
// a far larger gap than the unmatched closes that led here. libuct_ib_mlx5.so.0
// references mlx5dv_open_device, and this provider installs only from
// ibv_open_device, so that gap is possible; whether it is real is what this
// measures. An unmatched mr whose context we never installed into points at the
// opening door, not at the registration door.
static std::set<const void*>* g_installed_ctxs = nullptr;

// Which ibv_contexts currently have object-ops installed.
//
// Object-ops hooks live INSIDE a context: install() patches function pointers in
// one ibv_context, so a context we never install into is invisible to every one
// of them at once — post_send and poll_cq included, not just the ibv_reg_dm_mr
// that led here. Job 44369505 measured that: 262 unmatched closes, every one on
// a context this provider had never seen.
//
// Two rules this set exists to get right, both learned the hard way:
//
//  1. It MUST have a lifecycle. Keyed on a pointer, with no removal, it reports
//     "already installed" for a brand-new context that the allocator placed at a
//     freed one's address. That cost case 02 two of eight ranks — 26880 sends to
//     20160 — silently, and only on the ranks that happened to recycle. Hence
//     forget_context on ibv_close_device.
//
//  2. It must never make installation conditional on something OTHER than "is
//     this context already patched". Re-installing would save our own trampoline
//     as the original and recurse; not installing loses the capture. The set
//     answers exactly that question and nothing else.
// Which door installed each context, counted. See dump_install_doors: a context
// first seen at ibv_open_device has a zero-length blind window by construction,
// and one first seen anywhere else does not, so this histogram is the difference
// between "the window is theoretical" and "the window is real" — measured rather
// than reasoned from API ordering, which has been wrong here twice.
static std::map<std::string, unsigned>* g_install_doors = nullptr;  // leaked, like the sets

void ibv::ensure_installed(ibv_context* ctx, const char* door) {
    if (!ctx)
        return;
    {
        std::lock_guard<std::mutex> lk(g_known_mr_mutex);
        if (!g_installed_ctxs)
            g_installed_ctxs = new std::set<const void*>();
        if (!g_installed_ctxs->insert(ctx).second)
            return;  // already patched, and still alive
        if (!g_install_doors)
            g_install_doors = new std::map<std::string, unsigned>();
        ++(*g_install_doors)[door ? door : "?"];
    }
    ibv::Profiler::instance().install(ctx);
    record_device(ctx);
}

// The HCA behind this context, recorded once, here.
//
// This is the only spot in the provider that runs exactly once per context
// whatever door the context arrives at — which is the whole reason the row is
// emitted from here rather than from a hooked ibv_query_device. 48 of 73
// contexts per rank first reach us at ibv_alloc_pd (job 44396249), so a fact
// tied to any single API entry point is a fact most contexts never carry.
//
// We call ibv_query_device ourselves rather than waiting to observe someone
// else's call. That is the same move local_lid() makes with ibv_query_port on
// the modify_qp path: the fact is one call away and asking for it is cheaper
// than hoping the application asks. ibv_query_device IS a door we interpose, so
// this call re-enters our own interposer — harmless, because ensure_installed
// has already inserted this ctx into g_installed_ctxs by the time we get here
// and therefore returns immediately without recursing.
//
// A failed query still emits a row with zeroed identity: the context exists and
// its events will name it, so an entity that is present-but-unidentified beats
// one that is absent — the same bargain reg_dmabuf_mr strikes when it cannot
// resolve an address.
void ibv::record_device(ibv_context* ctx) {
    ibv_device_attr a{};
    const bool      ok = ibv_query_device(ctx, &a) == 0;
    rec().record(pcomm::core::Temporal::none(),
                 rr::IbDevice{{},
                              {},
                              h(ctx),
                              rec().intern(InternDomain::String, ok ? a.fw_ver : ""),
                              ok ? be64toh(a.node_guid) : 0ull,
                              ok ? be64toh(a.sys_image_guid) : 0ull,
                              ok ? a.vendor_id : 0u,
                              ok ? a.vendor_part_id : 0u,
                              ok ? a.hw_ver : 0u,
                              ok ? a.phys_port_cnt : std::uint8_t{0}});
}

// DIAGNOSTIC, gated on PCOMM_TRACE_CTX_INSTALL=1. One line per door that ever
// installed a context, plus the total — enough to answer whether any context
// reaches us later than ibv_open_device, which is the whole question.
//
// Note a context closed and reopened at the same address counts twice, which is
// correct: each is a separate context with its own blind window.
static void dump_install_doors() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_TRACE_CTX_INSTALL");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    if (!on)
        return;
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return;  // shutdown() and the unload destructor both call this
    std::lock_guard<std::mutex> lk(g_known_mr_mutex);
    unsigned                    total = 0, late = 0;
    if (g_install_doors)
        for (const auto& [door, n] : *g_install_doors) {
            total += n;
            if (door != "open_device")
                late += n;
            std::fprintf(stderr, "[pcomm-ctxinstall] door=%-14s contexts=%u\n", door.c_str(), n);
        }
    std::fprintf(stderr,
                 "[pcomm-ctxinstall] total=%u late=%u (late => a context ran uninstrumented "
                 "between its open and that door)\n",
                 total, late);
    std::fflush(stderr);
}

void ibv::forget_context(const ibv_context* ctx) {
    if (!ctx)
        return;
    std::lock_guard<std::mutex> lk(g_known_mr_mutex);
    if (g_installed_ctxs)
        g_installed_ctxs->erase(ctx);
}

// The CQ twin of remember_mr/trace_unmatched_dereg, and it exists because the
// receive plan's stage 1 uncovered the same shape of hole one layer down:
// ib_completion_queue closes EXACTLY 30 CQs per rank that nothing created, on
// all 8 ranks of case 02 (NCCL) and all 8 of case 07 (NVSHMEM multinode) alike.
// Two unrelated workloads producing an identical per-rank number is not a race
// and not an install-window artifact — those vary — so it points at a creation
// door nobody has named.
//
// IbCompletionQueue.origin narrows it from the capture side by saying which of
// our two doors each recorded CQ came through. This narrows it from the process
// side by naming the LIBRARY and FUNCTION that destroys a CQ we never saw
// created, and whether that CQ's context was ever installed — which is exactly
// the trio that settled the reg_mr hunt, where the per-door caller table alone
// could not tell "created elsewhere" from "created through a door we do not
// hook".
//
// Gated on PCOMM_TRACE_MR_CALLERS=1 like its sibling: same hunt, same switch.
static std::set<const void*>* g_known_cqs = nullptr;  // leaked, like the rest

void ibv::remember_cq(const ibv_cq* cq) {
    if (!cq || !trace_mr_callers_enabled())
        return;
    std::lock_guard<std::mutex> lk(g_known_mr_mutex);
    if (!g_known_cqs)
        g_known_cqs = new std::set<const void*>();
    g_known_cqs->insert(cq);
}

void ibv::trace_unmatched_destroy_cq(const ibv_cq* cq, void* ret_addr) {
    if (!cq || !trace_mr_callers_enabled())
        return;
    {
        std::lock_guard<std::mutex> lk(g_known_mr_mutex);
        if (g_known_cqs && g_known_cqs->count(cq))
            return;  // created through a door we hook — nothing to report
    }
    Dl_info     info{};
    const bool  ok = dladdr(ret_addr, &info) != 0;
    const char* who = (ok && info.dli_fname) ? info.dli_fname : "?";
    const char* fn = (ok && info.dli_sname) ? info.dli_sname : "?";
    bool        installed = false;
    {
        std::lock_guard<std::mutex> lk(g_known_mr_mutex);
        installed = g_installed_ctxs && cq->context && g_installed_ctxs->count(cq->context);
    }
    static std::mutex            m;
    std::lock_guard<std::mutex>  lk(m);
    static std::set<std::string> seen;
    std::string                  key = std::string(fn) + "|" + who + "|" + (installed ? "y" : "n");
    if (!seen.insert(key).second)
        return;  // one line per distinct caller, not per CQ
    std::fprintf(stderr,
                 "[pcomm-unmatched] destroy_cq cq=%p cqe=%d ctx=%p installed=%s <- %s in %s\n",
                 static_cast<const void*>(cq), cq->cqe, static_cast<const void*>(cq->context),
                 installed ? "yes" : "NO", fn, who);
    std::fflush(stderr);
}

static void remember_mr(const ibv_mr* mr) {
    if (!mr || !trace_mr_callers_enabled())
        return;
    std::lock_guard<std::mutex> lk(g_known_mr_mutex);
    if (!g_known_mrs)
        g_known_mrs = new std::set<const void*>();
    g_known_mrs->insert(mr);
}

static void trace_unmatched_dereg(const ibv_mr* mr, void* ret_addr) {
    if (!mr || !trace_mr_callers_enabled())
        return;
    {
        std::lock_guard<std::mutex> lk(g_known_mr_mutex);
        if (g_known_mrs && g_known_mrs->count(mr))
            return;  // opened through a door we hook — nothing to report
    }
    // dli_sname, not just dli_fname: the library was not enough. Both callers
    // are UCX, neither references any other ibv_mr*-returning symbol, and every
    // unmatched region has addr=NULL — so the useful question is which UCX
    // FUNCTION is closing them, which names the registration path that created
    // them. The symbol is the nearest preceding one, so read it as "in or after
    // this function", not as an exact frame.
    Dl_info     info{};
    const bool  ok = dladdr(ret_addr, &info) != 0;
    const char* who = (ok && info.dli_fname) ? info.dli_fname : "?";
    const char* fn = (ok && info.dli_sname) ? info.dli_sname : "?";
    // Was this mr's context one we installed into? If not, the door to find is
    // the one that OPENED the context, not the one that registered the memory.
    bool installed = false;
    {
        std::lock_guard<std::mutex> lk(g_known_mr_mutex);
        installed = g_installed_ctxs && mr->context && g_installed_ctxs->count(mr->context);
    }
    std::fprintf(stderr,
                 "[pcomm-unmatched] dereg mr=%p addr=%p len=%zu lkey=%u ctx=%p installed=%s <- %s "
                 "in %s\n",
                 static_cast<const void*>(mr), mr->addr, mr->length, mr->lkey,
                 static_cast<const void*>(mr->context), installed ? "yes" : "NO", fn, who);
    std::fflush(stderr);
}

extern "C" ibv_mr* ibv_reg_dmabuf_mr(ibv_pd* pd, uint64_t offset, size_t length, uint64_t iova,
                                     int fd, int access) {
    trace_mr_caller("reg_dmabuf_mr", __builtin_return_address(0), (long)length);
    if (pd)
        ibv::ensure_installed(pd->context, "reg_dmabuf_mr");
    auto* mr =
        ibv::Profiler::invoke_preload<ibv::reg_dmabuf_mr_tag>(pd, offset, length, iova, fd, access);
    remember_mr(mr);
    return mr;
}
extern "C" int ibv_dereg_mr(ibv_mr* mr) {
    trace_mr_caller("dereg_mr", __builtin_return_address(0), mr ? (long)mr->length : -1);
    if (mr)
        ibv::ensure_installed(mr->context, "dereg_mr");
    trace_unmatched_dereg(mr, __builtin_return_address(0));
    return ibv::Profiler::invoke_preload<ibv::dereg_mr_tag>(mr);
}

// The legacy registration door. The depth guard must wrap invoke_preload and
// not just the real call: the post callback runs inside it, and rdma-core's
// internal hop to iova2 (when it makes one) happens inside it too, so both must
// see the same nonzero depth. See reg_mr_depth().
// verbs.h defines ibv_reg_mr as a MACRO over a static inline that picks between
// the legacy symbol and iova2 by inspecting the access flags. Without this undef
// the definition below expands into that macro and does not compile — and the
// macro is also why "hook ibv_reg_mr" was never as simple as adding a name.
#undef ibv_reg_mr
extern "C" ibv_mr* ibv_reg_mr(ibv_pd* pd, void* addr, size_t length, int access) {
    trace_mr_caller("reg_mr", __builtin_return_address(0), (long)length);
    if (pd)
        ibv::ensure_installed(pd->context, "reg_mr");
    struct Depth {
        Depth() noexcept { ++ibv::reg_mr_depth(); }
        ~Depth() noexcept { --ibv::reg_mr_depth(); }
    } guard;
    auto* mr = ibv::Profiler::invoke_preload<ibv::reg_mr_tag>(pd, addr, length, access);
    remember_mr(mr);
    return mr;
}

// The iova2 door. Reached directly by callers whose access flags are not a
// compile-time constant, and indirectly from the legacy symbol above on
// rdma-core builds that route — where the depth guard makes it record nothing.
extern "C" ibv_mr* ibv_reg_mr_iova2(ibv_pd* pd, void* addr, size_t length, uint64_t iova,
                                    unsigned int access) {
    trace_mr_caller("reg_mr_iova2", __builtin_return_address(0), (long)length);
    if (pd)
        ibv::ensure_installed(pd->context, "reg_mr_iova2");
    auto* mr = ibv::Profiler::invoke_preload<ibv::reg_mr_iova2_tag>(pd, addr, length, iova, access);
    remember_mr(mr);
    return mr;
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
        dump_install_doors();
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
    dump_install_doors();
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
