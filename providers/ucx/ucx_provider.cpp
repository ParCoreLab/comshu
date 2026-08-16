#include <cstdint>

#include <ucp/api/ucp.h>
#include <ucs/type/status.h>

#include "pcomm/core/records.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/tracer.hpp"

#include "ucx_event_source.hpp"
#include "ucx_records.hpp"

namespace ucx {
namespace rr = pcomm::providers::ucx::records;
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

struct ClassifiedStatus {
    pcomm::core::Handle request{};
    std::int32_t       status{UCS_OK};
    rr::StatusClass    kind{rr::StatusClass::Immediate};
};

static ClassifiedStatus classify(ucs_status_ptr_t value) {
    if (value == nullptr)
        return {};
    if (UCS_PTR_IS_ERR(value))
        return {pcomm::core::kNullHandle, static_cast<std::int32_t>(UCS_PTR_STATUS(value)),
                rr::StatusClass::Error};
    return {h(value), UCS_INPROGRESS, rr::StatusClass::Request};
}

Profiler::Profiler() {
    register_post<init_version_tag>([](ucs_status_t status, unsigned major, unsigned minor,
                                       const ucp_params_t*, const ucp_config_t*, ucp_context_h* out,
                                       auto begin, auto end) {
        if (status == UCS_OK && out && *out)
            rec().record(span(begin, end),
                         rr::UcxContext{{}, {}, h(*out), major, minor});
    });
    register_pre<cleanup_tag>([](ucp_context_h context, auto) {
        close(context, pcomm::core::RecordTrait<rr::UcxContext>::type_id);
    });
    register_post<worker_create_tag>([](ucs_status_t status, ucp_context_h context,
                                        const ucp_worker_params_t*, ucp_worker_h* out, auto begin,
                                        auto end) {
        if (status == UCS_OK && out && *out)
            rec().record(span(begin, end),
                         rr::UcxWorker{{}, {}, h(*out), h(context)});
    });
    register_pre<worker_destroy_tag>([](ucp_worker_h worker, auto) {
        close(worker, pcomm::core::RecordTrait<rr::UcxWorker>::type_id);
    });
    register_post<ep_create_tag>([](ucs_status_t status, ucp_worker_h worker,
                                    const ucp_ep_params_t*, ucp_ep_h* out, auto begin, auto end) {
        if (status == UCS_OK && out && *out)
            rec().record(span(begin, end),
                         rr::UcxEndpoint{{}, {}, h(*out), h(worker)});
    });
    register_pre<ep_close_nbx_tag>([](ucp_ep_h endpoint, const ucp_request_param_t*, auto) {
        close(endpoint, pcomm::core::RecordTrait<rr::UcxEndpoint>::type_id);
    });
    register_post<tag_send_nbx_tag>([](ucs_status_ptr_t result, ucp_ep_h endpoint,
                                       const void* buffer, size_t count, ucp_tag_t tag,
                                       const ucp_request_param_t*, auto begin, auto end) {
        const auto s = classify(result);
        rec().record(span(begin, end),
                     rr::UcxTagSend{{},
                                    {},
                                    h(endpoint),
                                    h(buffer),
                                    s.request,
                                    count,
                                    tag,
                                    s.status,
                                    static_cast<std::uint8_t>(s.kind)});
    });
}

template class UcxEventSource<Profiler>;
}  // namespace ucx

extern "C" ucs_status_t ucp_init_version(unsigned major, unsigned minor, const ucp_params_t* params,
                                          const ucp_config_t* config, ucp_context_h* context) {
    return ucx::Profiler::invoke_preload<ucx::init_version_tag>(major, minor, params, config, context);
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
extern "C" ucs_status_ptr_t ucp_ep_close_nbx(ucp_ep_h endpoint,
                                              const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::ep_close_nbx_tag>(endpoint, params);
}
extern "C" ucs_status_ptr_t ucp_tag_send_nbx(ucp_ep_h endpoint, const void* buffer, size_t count,
                                              ucp_tag_t tag,
                                              const ucp_request_param_t* params) {
    return ucx::Profiler::invoke_preload<ucx::tag_send_nbx_tag>(endpoint, buffer, count, tag, params);
}

namespace pcomm::providers {
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
        pcomm_engine_drain_thread();
        pcomm_engine_release();
    }
};
}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::UcxProvider)

static void __attribute__((constructor)) pcomm_ucx_load() { ucx::Profiler::instance(); }
static void __attribute__((destructor)) pcomm_ucx_unload() {
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
