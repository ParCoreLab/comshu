#pragma once

#include <ucp/api/ucp.h>

#include "pcomm/hook/HookTrait.hpp"

DEFINE_PRELOAD_HOOK(ucx, init_version, "ucp_init_version", ucs_status_t, unsigned, unsigned,
                    const ucp_params_t*, const ucp_config_t*, ucp_context_h*);
DEFINE_PRELOAD_HOOK(ucx, cleanup, "ucp_cleanup", void, ucp_context_h);
DEFINE_PRELOAD_HOOK(ucx, worker_create, "ucp_worker_create", ucs_status_t, ucp_context_h,
                    const ucp_worker_params_t*, ucp_worker_h*);
DEFINE_PRELOAD_HOOK(ucx, worker_destroy, "ucp_worker_destroy", void, ucp_worker_h);
DEFINE_PRELOAD_HOOK(ucx, ep_create, "ucp_ep_create", ucs_status_t, ucp_worker_h,
                    const ucp_ep_params_t*, ucp_ep_h*);
DEFINE_PRELOAD_HOOK(ucx, ep_close_nbx, "ucp_ep_close_nbx", ucs_status_ptr_t, ucp_ep_h,
                    const ucp_request_param_t*);
DEFINE_PRELOAD_HOOK(ucx, tag_send_nbx, "ucp_tag_send_nbx", ucs_status_ptr_t, ucp_ep_h,
                    const void*, size_t, ucp_tag_t, const ucp_request_param_t*);
