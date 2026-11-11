#pragma once

#include "comshu/hook/CompositeHookManager.hpp"
#include "ucx_hooks.hpp"

namespace ucx {

template<typename Derived>
class UcxEventSource
    : public comshu::hook::CompositeHookManager<
          Derived, comshu::hook::DefaultHookManagerConfig, init_version_tag, cleanup_tag,
          worker_create_tag, worker_destroy_tag, ep_create_tag, ep_close_nbx_tag, tag_send_nbx_tag,
          tag_recv_nbx_tag, put_nbx_tag, get_nbx_tag, am_send_nbx_tag, mem_map_tag, mem_unmap_tag,
          worker_progress_tag, ep_close_nb_tag, ep_destroy_tag, disconnect_nb_tag, put_nbi_tag,
          get_nbi_tag, get_nb_tag, tag_send_nb_tag, tag_send_sync_nb_tag, tag_recv_nb_tag,
          tag_probe_nb_tag, tag_msg_recv_nb_tag, atomic_post_tag, atomic_fetch_nb_tag,
          atomic_op_nbx_tag, worker_get_address_tag, worker_query_tag, ep_rkey_unpack_tag,
          rkey_pack_tag, rkey_destroy_tag, ep_rkey_unpack_internal_tag> {
    using Base = comshu::hook::CompositeHookManager<
        Derived, comshu::hook::DefaultHookManagerConfig, init_version_tag, cleanup_tag,
        worker_create_tag, worker_destroy_tag, ep_create_tag, ep_close_nbx_tag, tag_send_nbx_tag,
        tag_recv_nbx_tag, put_nbx_tag, get_nbx_tag, am_send_nbx_tag, mem_map_tag, mem_unmap_tag,
        worker_progress_tag, ep_close_nb_tag, ep_destroy_tag, disconnect_nb_tag, put_nbi_tag,
        get_nbi_tag, get_nb_tag, tag_send_nb_tag, tag_send_sync_nb_tag, tag_recv_nb_tag,
        tag_probe_nb_tag, tag_msg_recv_nb_tag, atomic_post_tag, atomic_fetch_nb_tag,
        atomic_op_nbx_tag, worker_get_address_tag, worker_query_tag, ep_rkey_unpack_tag,
        rkey_pack_tag, rkey_destroy_tag, ep_rkey_unpack_internal_tag>;

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
