#pragma once

#include "pcomm/hook/CompositeHookManager.hpp"

#include "ucx_deep_hooks.hpp"

namespace ucx_deep {

template<typename Derived>
class UctEventSource
    : public pcomm::hook::CompositeHookManager<Derived, pcomm::hook::DefaultHookManagerConfig,
                                               uctd::iface_open_tag> {
    using Base = pcomm::hook::CompositeHookManager<Derived, pcomm::hook::DefaultHookManagerConfig,
                                                   uctd::iface_open_tag>;

public:
    using Context = typename Base::Context;
    static void* GetLibHandle() noexcept { return nullptr; }

protected:
    UctEventSource() = default;
};

struct Profiler : UctEventSource<Profiler> {
    Profiler();
};
extern template class UctEventSource<Profiler>;

}  // namespace ucx_deep
