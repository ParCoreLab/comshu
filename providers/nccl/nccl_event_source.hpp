#pragma once

#include "nccl_hooks.hpp"
#include "comshu/hook/CompositeHookManager.hpp"

namespace nccl {

// ncclGroupStart/End are absent from this pack on purpose — they are nullary
// and interposed by hand. See nccl_hooks.hpp.
//
// The window/devcomm tags exist only where the header has the symmetric-memory
// API (NCCL >= 2.27). Spelling the pack twice behind one macro keeps the two
// mentions of it — the base class and the using-alias — from drifting apart,
// which is a class of bug the UCX source has to be careful about by hand.
#if COMSHU_NCCL_HAS_WINDOW
#define COMSHU_NCCL_TAGS                                                                            \
    comm_init_rank_tag, comm_init_rank_config_tag, comm_destroy_tag, comm_abort_tag,               \
        all_reduce_tag, broadcast_tag, reduce_tag, all_gather_tag, reduce_scatter_tag, send_tag,   \
        recv_tag, window_register_tag, window_deregister_tag, dev_comm_create_tag
#else
#define COMSHU_NCCL_TAGS                                                                            \
    comm_init_rank_tag, comm_init_rank_config_tag, comm_destroy_tag, comm_abort_tag,               \
        all_reduce_tag, broadcast_tag, reduce_tag, all_gather_tag, reduce_scatter_tag, send_tag,   \
        recv_tag
#endif

template<typename Derived>
class NcclEventSource
    : public comshu::hook::CompositeHookManager<Derived, comshu::hook::DefaultHookManagerConfig,
                                               COMSHU_NCCL_TAGS> {
    using Base = comshu::hook::CompositeHookManager<Derived, comshu::hook::DefaultHookManagerConfig,
                                                   COMSHU_NCCL_TAGS>;

public:
    using Context = typename Base::Context;
    static void* GetLibHandle() noexcept { return nullptr; }

protected:
    NcclEventSource() = default;
};

struct Profiler : NcclEventSource<Profiler> {
    Profiler();
};
extern template class NcclEventSource<Profiler>;

}  // namespace nccl
