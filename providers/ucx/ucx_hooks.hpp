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
// The other half of the tagged path, and the half a capture is useless without:
// with sends alone a ping-pong shows one direction and every latency question
// has nowhere to land. Takes a WORKER, not an endpoint — a receive is matched by
// tag across everything the worker can hear from, so there is no endpoint to
// name and ucx_tag_recv deliberately does not invent one.
DEFINE_PRELOAD_HOOK(ucx, tag_recv_nbx, "ucp_tag_recv_nbx", ucs_status_ptr_t, ucp_worker_h, void*,
                    size_t, ucp_tag_t, ucp_tag_t, const ucp_request_param_t*);
// One-sided put and get. Two symbols, one record (ucx_rma) discriminated by an
// op field, on the reasoning ib_post_send already follows for send/write/read: a
// reader asking "what one-sided traffic did this endpoint issue" should not have
// to union two tables to find out.
//
// Neither fired in the Stage 0 MPI arm (jobs 44629699/44629730), which is a fact
// about that workload and not about the API — it is a tagged ping-pong plus an
// all-reduce. They are hooked because the moment a workload uses MPI RMA or a
// PGAS layer over UCX, they become the whole story.
DEFINE_PRELOAD_HOOK(ucx, put_nbx, "ucp_put_nbx", ucs_status_ptr_t, ucp_ep_h, const void*, size_t,
                    uint64_t, ucp_rkey_h, const ucp_request_param_t*);
DEFINE_PRELOAD_HOOK(ucx, get_nbx, "ucp_get_nbx", ucs_status_ptr_t, ucp_ep_h, void*, size_t,
                    uint64_t, ucp_rkey_h, const ucp_request_param_t*);
// Active messages. Carries TWO lengths — a header and a payload — and they are
// kept apart in the record rather than summed, because an AM protocol's header
// count is control traffic and its payload is data.
DEFINE_PRELOAD_HOOK(ucx, am_send_nbx, "ucp_am_send_nbx", ucs_status_ptr_t, ucp_ep_h, unsigned,
                    const void*, size_t, const void*, size_t, const ucp_request_param_t*);
// Memory registration through UCP rather than through verbs. The ibverbs
// provider sees the ibv_reg_mr underneath this, so the two are not redundant:
// this one says which registrations the APPLICATION asked UCX for, and that is
// the only layer at which a ucp_mem_h means anything.
DEFINE_PRELOAD_HOOK(ucx, mem_map, "ucp_mem_map", ucs_status_t, ucp_context_h,
                    const ucp_mem_map_params_t*, ucp_mem_h*);
DEFINE_PRELOAD_HOOK(ucx, mem_unmap, "ucp_mem_unmap", ucs_status_t, ucp_context_h, ucp_mem_h);
// Progress, and the ONLY hook here that needs a predicate. Stage 0 measured
// 329,073 to 617,094 progress calls per rank against 133 sends, and the counts
// varied by a factor of two between two runs of the identical workload — so this
// is telemetry, never a golden, and recording every call would make the progress
// stream several thousand times larger than every other UCX stream combined.
//
// A call that progressed nothing is a poll that found an empty queue, which is
// the same firehose ibv_poll_cq's predicate exists to cut. Keeping only
// num_progressed > 0 leaves the calls that did something. It is still Full-mode
// only — see record_progress() in the provider.
DEFINE_PRELOAD_HOOK_PRED(
    ucx, worker_progress, "ucp_worker_progress",
    [](unsigned progressed, ucp_worker_h) { return progressed > 0; }, unsigned, ucp_worker_h);
