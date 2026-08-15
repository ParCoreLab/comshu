// Stage-0 UCX coverage probe. This is deliberately not the pcomm provider: it
// answers which public UCP doors the site MPI actually crosses before the
// capture schema is committed. Build as a preload library against the site's
// UCX headers.

#include <ucp/api/ucp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>

namespace {

enum Call : std::size_t {
    Init,
    Cleanup,
    WorkerCreate,
    WorkerDestroy,
    EpCreate,
    EpClose,
    TagSend,
    TagRecv,
    Put,
    Get,
    AmSend,
    MemMap,
    MemUnmap,
    WorkerProgress,
    Count,
};

constexpr std::array<const char*, Count> kNames{
    "ucp_init_version", "ucp_cleanup",         "ucp_worker_create", "ucp_worker_destroy",
    "ucp_ep_create",    "ucp_ep_close_nbx",    "ucp_tag_send_nbx",  "ucp_tag_recv_nbx",
    "ucp_put_nbx",      "ucp_get_nbx",         "ucp_am_send_nbx",   "ucp_mem_map",
    "ucp_mem_unmap",    "ucp_worker_progress",
};

std::array<std::atomic<unsigned long long>, Count> g_calls{};
std::array<std::atomic<unsigned long long>, Count> g_count{};
std::atomic<unsigned long long>                    g_next_scope{1};

struct Frame {
    std::uint32_t      call;
    unsigned long long id;
};
thread_local std::array<Frame, 16> g_scopes{};
thread_local std::size_t           g_scope_depth{};

struct Scope {
    explicit Scope(Call call) {
        if (g_scope_depth < g_scopes.size())
            g_scopes[g_scope_depth++] = {
                static_cast<std::uint32_t>(call),
                g_next_scope.fetch_add(1, std::memory_order_relaxed),
            };
    }
    ~Scope() {
        if (g_scope_depth)
            --g_scope_depth;
    }
};

template<typename Fn> Fn real(const char* name) {
    void* p = dlsym(RTLD_NEXT, name);
    if (!p) {
        std::fprintf(stderr, "[ucx-api] pid=%ld cannot resolve %s: %s\n", (long)getpid(), name,
                     dlerror());
        std::abort();
    }
    return reinterpret_cast<Fn>(p);
}

void hit(Call call, std::size_t count = 0) {
    g_calls[call].fetch_add(1, std::memory_order_relaxed);
    g_count[call].fetch_add(count, std::memory_order_relaxed);
}

__attribute__((destructor)) void report() {
    std::fprintf(stderr, "[ucx-api] pid=%ld summary", (long)getpid());
    for (std::size_t i = 0; i < Count; ++i) {
        const auto calls = g_calls[i].load(std::memory_order_relaxed);
        if (calls)
            std::fprintf(stderr, " %s=%llu(count=%llu)", kNames[i], calls,
                         g_count[i].load(std::memory_order_relaxed));
    }
    std::fputc('\n', stderr);
}

}  // namespace

// Probe-only bridge consumed by the ibverbs provider when
// PCOMM_TRACE_UCX_NESTING=1. Returning the innermost frame matters if UCX calls
// progress from inside an operation: the verbs post belongs to what is actually
// on the stack, not to a permanently cached endpoint guess.
extern "C" int pcomm_ucx_api_probe_current(std::uint32_t* call, unsigned long long* id) {
    if (!g_scope_depth)
        return 0;
    const Frame& frame = g_scopes[g_scope_depth - 1];
    if (call)
        *call = frame.call;
    if (id)
        *id = frame.id;
    return 1;
}

extern "C" ucs_status_t ucp_init_version(unsigned major, unsigned minor, const ucp_params_t* params,
                                         const ucp_config_t* config, ucp_context_h* context) {
    using Fn = decltype(&ucp_init_version);
    static Fn fn = real<Fn>("ucp_init_version");
    hit(Init);
    return fn(major, minor, params, config, context);
}

extern "C" void ucp_cleanup(ucp_context_h context) {
    using Fn = decltype(&ucp_cleanup);
    static Fn fn = real<Fn>("ucp_cleanup");
    hit(Cleanup);
    fn(context);
}

extern "C" ucs_status_t ucp_worker_create(ucp_context_h context, const ucp_worker_params_t* params,
                                          ucp_worker_h* worker) {
    using Fn = decltype(&ucp_worker_create);
    static Fn fn = real<Fn>("ucp_worker_create");
    hit(WorkerCreate);
    return fn(context, params, worker);
}

extern "C" void ucp_worker_destroy(ucp_worker_h worker) {
    using Fn = decltype(&ucp_worker_destroy);
    static Fn fn = real<Fn>("ucp_worker_destroy");
    hit(WorkerDestroy);
    fn(worker);
}

extern "C" ucs_status_t ucp_ep_create(ucp_worker_h worker, const ucp_ep_params_t* params,
                                      ucp_ep_h* ep) {
    using Fn = decltype(&ucp_ep_create);
    static Fn fn = real<Fn>("ucp_ep_create");
    hit(EpCreate);
    return fn(worker, params, ep);
}

extern "C" ucs_status_ptr_t ucp_ep_close_nbx(ucp_ep_h ep, const ucp_request_param_t* param) {
    using Fn = decltype(&ucp_ep_close_nbx);
    static Fn fn = real<Fn>("ucp_ep_close_nbx");
    hit(EpClose);
    return fn(ep, param);
}

extern "C" ucs_status_ptr_t ucp_tag_send_nbx(ucp_ep_h ep, const void* buffer, std::size_t count,
                                             ucp_tag_t tag, const ucp_request_param_t* param) {
    using Fn = decltype(&ucp_tag_send_nbx);
    static Fn fn = real<Fn>("ucp_tag_send_nbx");
    hit(TagSend, count);
    Scope scope(TagSend);
    return fn(ep, buffer, count, tag, param);
}

extern "C" ucs_status_ptr_t ucp_tag_recv_nbx(ucp_worker_h worker, void* buffer, std::size_t count,
                                             ucp_tag_t tag, ucp_tag_t mask,
                                             const ucp_request_param_t* param) {
    using Fn = decltype(&ucp_tag_recv_nbx);
    static Fn fn = real<Fn>("ucp_tag_recv_nbx");
    hit(TagRecv, count);
    Scope scope(TagRecv);
    return fn(worker, buffer, count, tag, mask, param);
}

extern "C" ucs_status_ptr_t ucp_put_nbx(ucp_ep_h ep, const void* buffer, std::size_t count,
                                        std::uint64_t remote_addr, ucp_rkey_h rkey,
                                        const ucp_request_param_t* param) {
    using Fn = decltype(&ucp_put_nbx);
    static Fn fn = real<Fn>("ucp_put_nbx");
    hit(Put, count);
    Scope scope(Put);
    return fn(ep, buffer, count, remote_addr, rkey, param);
}

extern "C" ucs_status_ptr_t ucp_get_nbx(ucp_ep_h ep, void* buffer, std::size_t count,
                                        std::uint64_t remote_addr, ucp_rkey_h rkey,
                                        const ucp_request_param_t* param) {
    using Fn = decltype(&ucp_get_nbx);
    static Fn fn = real<Fn>("ucp_get_nbx");
    hit(Get, count);
    Scope scope(Get);
    return fn(ep, buffer, count, remote_addr, rkey, param);
}

extern "C" ucs_status_ptr_t ucp_am_send_nbx(ucp_ep_h ep, unsigned id, const void* header,
                                            std::size_t header_length, const void* buffer,
                                            std::size_t count, const ucp_request_param_t* param) {
    using Fn = decltype(&ucp_am_send_nbx);
    static Fn fn = real<Fn>("ucp_am_send_nbx");
    hit(AmSend, count);
    Scope scope(AmSend);
    return fn(ep, id, header, header_length, buffer, count, param);
}

extern "C" ucs_status_t ucp_mem_map(ucp_context_h context, const ucp_mem_map_params_t* params,
                                    ucp_mem_h* mem) {
    using Fn = decltype(&ucp_mem_map);
    static Fn fn = real<Fn>("ucp_mem_map");
    hit(MemMap);
    return fn(context, params, mem);
}

extern "C" ucs_status_t ucp_mem_unmap(ucp_context_h context, ucp_mem_h mem) {
    using Fn = decltype(&ucp_mem_unmap);
    static Fn fn = real<Fn>("ucp_mem_unmap");
    hit(MemUnmap);
    return fn(context, mem);
}

extern "C" unsigned ucp_worker_progress(ucp_worker_h worker) {
    using Fn = decltype(&ucp_worker_progress);
    static Fn fn = real<Fn>("ucp_worker_progress");
    hit(WorkerProgress);
    Scope scope(WorkerProgress);
    return fn(worker);
}
