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

// --- the doors MN5's stack actually references ------------------------------
//
// Everything above this line is the surface SigmaTrace hooks. That turned out to
// be the wrong target, and the measurement is worth stating because it inverts
// the obvious assumption: the modern `_nbx` API is NOT what most of MN5's stack
// calls. Read from the ELF dynamic symbol tables of the installed HPC-X 2.22.1
// libraries (2026-08-16, alogin2, no allocation needed — every one of these is
// an UND reference, so the code path exists in a library already measured to be
// loaded):
//
//   mca_pml_ucx.so   MPI point-to-point  ucp_tag_send_nb, ucp_tag_send_sync_nb,
//                                        ucp_tag_recv_nb, ucp_tag_msg_recv_nb
//                                        (AND the _nbx pair)
//   mca_osc_ucx.so   MPI one-sided       ucp_put_nbi, ucp_get_nbi,
//                                        ucp_atomic_post, ucp_atomic_fetch_nb
//                                        — and NO _nbx RMA call at all
//   mca_spml_ucx.so  OpenSHMEM           ucp_put_nbx, ucp_get_nbx, ucp_ep_destroy
//   hmca_bcol_ucx_p2p.so  hcoll          ucp_tag_send_nb, ucp_tag_recv_nb,
//                                        ucp_ep_close_nb
//   libsmx-3.10.so   SHARP               ucp_tag_send_nb, ucp_tag_msg_recv_nb
//   libnccl-net.so   NCCL plugin         ucp_am_send_nb, ucp_get_nb, ucp_put_nbi
//   libucc_tl_ucp.so UCC collectives     the _nbx set (already covered)
//
// The consequence that matters: **MPI RMA never touches ucp_put_nbx or
// ucp_get_nbx on this machine.** A workload built on MPI_Put/MPI_Get would have
// produced an empty ucx_rma table and looked like a transport that did no
// one-sided traffic, which is the worst kind of wrong — a confident zero.
//
// These are hooked on a static reference rather than on an observed call, which
// is weaker evidence than a probe run and much stronger than "the API exists".
// The asymmetry justifies it: hooking a door that never fires costs one dlsym,
// while missing one costs a silently incomplete capture — and for the endpoint
// close doors below it costs entity accounting outright.

// THREE more ways to close an endpoint, and ucx_endpoint was leaking on all of
// them. Only ucp_ep_close_nbx was hooked, while hcoll and the NCCL plugin close
// through ucp_ep_close_nb, OpenSHMEM and MPI one-sided through ucp_ep_destroy,
// and libmca_common_ucx through ucp_disconnect_nb. An entity whose open is
// recorded and whose close is not stays live forever in the capture, and since
// the allocator reuses the address, the NEXT endpoint at that address is
// indistinguishable from it — the exact defect ibv_close_device and
// ibv_dereg_mr exist to prevent, which cost case 02 two of its eight ranks.
DEFINE_PRELOAD_HOOK(ucx, ep_close_nb, "ucp_ep_close_nb", ucs_status_ptr_t, ucp_ep_h, unsigned);
DEFINE_PRELOAD_HOOK(ucx, ep_destroy, "ucp_ep_destroy", void, ucp_ep_h);
DEFINE_PRELOAD_HOOK(ucx, disconnect_nb, "ucp_disconnect_nb", ucs_status_ptr_t, ucp_ep_h);

// The one-sided doors MPI actually uses. `_nbi` is blocking-immediate: it
// returns a plain status and never a request, so a row from here is complete
// when it is written — which is why UcxDoor rides on the record rather than
// being inferred from status_class.
DEFINE_PRELOAD_HOOK(ucx, put_nbi, "ucp_put_nbi", ucs_status_t, ucp_ep_h, const void*, size_t,
                    uint64_t, ucp_rkey_h);
DEFINE_PRELOAD_HOOK(ucx, get_nbi, "ucp_get_nbi", ucs_status_t, ucp_ep_h, void*, size_t, uint64_t,
                    ucp_rkey_h);
DEFINE_PRELOAD_HOOK(ucx, get_nb, "ucp_get_nb", ucs_status_ptr_t, ucp_ep_h, void*, size_t, uint64_t,
                    ucp_rkey_h, ucp_send_callback_t);

// The legacy tagged doors. `ucp_tag_send_sync_nb` is not a duplicate of
// `ucp_tag_send_nb`: it is what MPI_Ssend becomes, completing only once the
// receiver has matched, so folding the two together would erase the difference
// between a send that buffered and a send that waited for its peer. UcxDoor
// keeps them apart.
DEFINE_PRELOAD_HOOK(ucx, tag_send_nb, "ucp_tag_send_nb", ucs_status_ptr_t, ucp_ep_h, const void*,
                    size_t, ucp_datatype_t, ucp_tag_t, ucp_send_callback_t);
DEFINE_PRELOAD_HOOK(ucx, tag_send_sync_nb, "ucp_tag_send_sync_nb", ucs_status_ptr_t, ucp_ep_h,
                    const void*, size_t, ucp_datatype_t, ucp_tag_t, ucp_send_callback_t);
DEFINE_PRELOAD_HOOK(ucx, tag_recv_nb, "ucp_tag_recv_nb", ucs_status_ptr_t, ucp_worker_h, void*,
                    size_t, ucp_datatype_t, ucp_tag_t, ucp_tag_t, ucp_tag_recv_callback_t);

// The probe/msg_recv pair, and the reason they are hooked TOGETHER.
//
// ucp_tag_msg_recv_nb carries a ucp_tag_message_h and no tag at all, so on its
// own it cannot fill ucx_tag_recv's key without inventing one. The tag is not
// lost, though — it was handed to the caller by the probe that produced the
// handle, in ucp_tag_recv_info_t. So the probe is hooked as the tag's source and
// the two are joined by the message handle in the reader.
//
// Validated end to end on the self transport before being written: a probe
// returned msg=0x1fe23d40 with sender_tag=0xabcd and length=48, and
// ucp_tag_msg_recv_nb was then called with that exact handle. The handle is an
// exact key; the join is temporal because UCX recycles it (see UcxTagRecv).
//
// Probes that find nothing are dropped at the hook. A probe loop spins exactly
// like an empty ibv_poll_cq, and the same predicate answer applies.
DEFINE_PRELOAD_HOOK_PRED(
    ucx, tag_probe_nb, "ucp_tag_probe_nb",
    [](ucp_tag_message_h found, ucp_worker_h, ucp_tag_t, ucp_tag_t, int,
       ucp_tag_recv_info_t*) { return found != nullptr; },
    ucp_tag_message_h, ucp_worker_h, ucp_tag_t, ucp_tag_t, int, ucp_tag_recv_info_t*);
DEFINE_PRELOAD_HOOK(ucx, tag_msg_recv_nb, "ucp_tag_msg_recv_nb", ucs_status_ptr_t, ucp_worker_h,
                    void*, size_t, ucp_datatype_t, ucp_tag_message_h, ucp_tag_recv_callback_t);

// The atomics. mca_osc_ucx (MPI one-sided) uses the first two, libucc_tl_ucp and
// mca_atomic_ucx the third — so without these an MPI RMA capture is missing the
// operations that make it RMA. Three doors, three DIFFERENT opcode numberings;
// see UcxAtomicOp for why they are normalised rather than stored raw.
DEFINE_PRELOAD_HOOK(ucx, atomic_post, "ucp_atomic_post", ucs_status_t, ucp_ep_h,
                    ucp_atomic_post_op_t, uint64_t, size_t, uint64_t, ucp_rkey_h);
DEFINE_PRELOAD_HOOK(ucx, atomic_fetch_nb, "ucp_atomic_fetch_nb", ucs_status_ptr_t, ucp_ep_h,
                    ucp_atomic_fetch_op_t, uint64_t, void*, size_t, uint64_t, ucp_rkey_h,
                    ucp_send_callback_t);
DEFINE_PRELOAD_HOOK(ucx, atomic_op_nbx, "ucp_atomic_op_nbx", ucs_status_ptr_t, ucp_ep_h,
                    ucp_atomic_op_t, const void*, size_t, uint64_t, ucp_rkey_h,
                    const ucp_request_param_t*);
