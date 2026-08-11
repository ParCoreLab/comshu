#pragma once
// Mlx5dvEventSource — binds the mlx5dv hook tags to pcomm's interception
// engine. Mirrors ib_event_source.hpp, minus the object-ops contract: DEVX has
// no ibv_context->ops equivalent, so every hook here is a preload interposer.

#include <string_view>

#include "pcomm/hook/CompositeHookManager.hpp"

#include "mlx5_hooks.hpp"

namespace mlx5 {
constexpr std::string_view LibPath{"libmlx5.so"};

template<typename Derived>
class Mlx5dvEventSource
    : public pcomm::hook::CompositeHookManager<
          Derived, pcomm::hook::DefaultHookManagerConfig, devx_umem_reg_tag, devx_umem_reg_ex_tag,
          devx_umem_dereg_tag, devx_alloc_uar_tag, devx_free_uar_tag, devx_obj_create_tag,
          devx_obj_modify_tag, create_qp_tag> {
    using Base = pcomm::hook::CompositeHookManager<
        Derived, pcomm::hook::DefaultHookManagerConfig, devx_umem_reg_tag, devx_umem_reg_ex_tag,
        devx_umem_dereg_tag, devx_alloc_uar_tag, devx_free_uar_tag, devx_obj_create_tag,
        devx_obj_modify_tag, create_qp_tag>;

public:
    using Context = typename Base::Context;

    static void* GetLibHandle() noexcept { return nullptr; }

protected:
    Mlx5dvEventSource() = default;
};

// The provider singleton; template instantiated once in mlx5dv_provider.cpp.
struct Profiler : Mlx5dvEventSource<Profiler> {
    Profiler();
};
extern template class Mlx5dvEventSource<Profiler>;

}  // namespace mlx5
