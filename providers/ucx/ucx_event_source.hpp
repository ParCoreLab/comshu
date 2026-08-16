#pragma once

#include "pcomm/hook/CompositeHookManager.hpp"
#include "ucx_hooks.hpp"

namespace ucx {

template<typename Derived>
class UcxEventSource
    : public pcomm::hook::CompositeHookManager<
          Derived, pcomm::hook::DefaultHookManagerConfig, init_version_tag, cleanup_tag,
          worker_create_tag, worker_destroy_tag, ep_create_tag, ep_close_nbx_tag,
          tag_send_nbx_tag> {
    using Base = pcomm::hook::CompositeHookManager<
        Derived, pcomm::hook::DefaultHookManagerConfig, init_version_tag, cleanup_tag,
        worker_create_tag, worker_destroy_tag, ep_create_tag, ep_close_nbx_tag, tag_send_nbx_tag>;

public:
    using Context = typename Base::Context;
    static void* GetLibHandle() noexcept { return nullptr; }

protected:
    UcxEventSource() = default;
};

struct Profiler : UcxEventSource<Profiler> {
    Profiler();
};
extern template class UcxEventSource<Profiler>;

}  // namespace ucx
