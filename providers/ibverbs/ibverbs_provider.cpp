// ibverbs proof provider — the first network provider on pcomm's core. Ported
// from SigmaTrace/Ibtrace: the hook engine (pcomm::hook) drives interposition;
// the callbacks below translate ibv_* handles/args into vendor-neutral pcomm
// records (ib_records.hpp, 100-range) through the Tracer's recorder.
//
// Object-ops hooks (post_send/poll_cq) are installed into ibv_context->ops on
// open_device; the rest are LD_PRELOAD interposers at the bottom of the file.

#include <cstdint>
#include <cstdlib>

#include "pcomm/core/ids.hpp"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/tracer.hpp"

#include "ib_event_source.hpp"
#include "ib_records.hpp"

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

// Interval [call-start, call-end] from the hook engine's captured rdtsc stamps
// (same clock domain as the recorder + clock anchors), so post_send/poll_cq
// render as slices with real durations instead of instants.
static pcomm::core::Temporal span(std::uint64_t t0, std::uint64_t t1) {
    return pcomm::core::Temporal::interval({0, t0}, {0, t1});
}

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
        if (ctx) instance().install(ctx);
    });

    // reg_mr -> memory-region entity.
    this->register_post<reg_mr_tag>(
        [](ibv_mr* mr, ibv_pd* pd, void* addr, size_t length, int access, auto...) {
            if (!mr) return;
            rec().record(pcomm::core::Temporal::none(),
                         rr::IbMemRegion{{}, {}, h(mr), h(pd),
                                         {reinterpret_cast<std::uint64_t>(addr), length},
                                         mr->lkey, mr->rkey,
                                         static_cast<std::uint32_t>(access)});
        });

    // create_qp -> queue-pair endpoint entity.
    this->register_post<create_qp_tag>([](ibv_qp* qp, ibv_pd* pd, ibv_qp_init_attr* a, auto...) {
        if (!qp) return;
        rec().record(pcomm::core::Temporal::none(),
                     rr::IbQueuePair{{}, {}, h(qp), h(pd), qp->qp_num,
                                     static_cast<std::uint8_t>(a ? a->qp_type : qp->qp_type)});
    });

    // modify_qp -> state transition; ->RTR carries the remote endpoint.
    this->register_post<modify_qp_tag>([](int ret, ibv_qp* qp, ibv_qp_attr* attr, int mask, auto...) {
        if (ret != 0 || !qp || !attr) return;
        const bool to_rtr = (mask & IBV_QP_STATE) && attr->qp_state == IBV_QPS_RTR;
        rec().record(rr::IbModifyQp{
            {}, {}, h(qp), qp->qp_num,
            to_rtr ? attr->dest_qp_num : 0u,
            to_rtr ? static_cast<std::uint32_t>(attr->ah_attr.dlid) : 0u,
            static_cast<std::uint8_t>((mask & IBV_QP_STATE) ? attr->qp_state : 0xFF)});
    });

    // post_send -> one row per work request in the chain (the comm firehose).
    // Records carry the call's [start, end] interval for slice rendering.
    this->register_post<post_send_tag>(
        [](int ret, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr**, std::uint64_t t0, std::uint64_t t1) {
            if (ret != 0 || !qp) return;
            const auto tspan = span(t0, t1);
            for (; wr; wr = wr->next) {
                std::uint32_t length = 0;
                for (int i = 0; i < wr->num_sge; ++i) length += wr->sg_list[i].length;
                std::uint64_t raddr = 0;
                std::uint32_t rkey = 0;
                if (wr->opcode == IBV_WR_RDMA_WRITE || wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM ||
                    wr->opcode == IBV_WR_RDMA_READ) {
                    raddr = wr->wr.rdma.remote_addr;
                    rkey = wr->wr.rdma.rkey;
                }
                rec().record(tspan, rr::IbPostSend{{}, {}, h(qp), wr->wr_id, raddr, rkey, length,
                                                   qp->qp_num, static_cast<std::uint8_t>(wr->opcode),
                                                   static_cast<std::uint8_t>(wr->num_sge)});
            }
        });

    // poll_cq -> one row per reaped completion (predicate skips empty polls).
    this->register_post<poll_cq_tag>(
        [](int ret, ibv_cq* cq, int, ibv_wc* wc, std::uint64_t t0, std::uint64_t t1) {
            if (ret <= 0 || !wc) return;
            const auto tspan = span(t0, t1);
            for (int i = 0; i < ret; ++i) {
                rec().record(tspan, rr::IbPollCq{{}, {}, h(cq), wc[i].wr_id, wc[i].byte_len,
                                                 wc[i].qp_num, static_cast<std::uint8_t>(wc[i].status),
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

// ibv_reg_mr is a macro in verbs.h (dispatches to iova2); interpose the plain
// symbol the ABI actually exports.
#ifdef ibv_reg_mr
#undef ibv_reg_mr
#endif
extern "C" ibv_mr* ibv_reg_mr(ibv_pd* pd, void* addr, size_t length, int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_mr_tag>(pd, addr, length, access);
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
static void __attribute__((constructor)) pcomm_ibverbs_load() { ibv::Profiler::instance(); }
static void __attribute__((destructor)) pcomm_ibverbs_unload() {
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
