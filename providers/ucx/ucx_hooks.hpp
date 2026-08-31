#pragma once

#include <ucp/api/ucp.h>

#include <cstdint>

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
DEFINE_PRELOAD_HOOK(ucx, tag_send_nbx, "ucp_tag_send_nbx", ucs_status_ptr_t, ucp_ep_h, const void*,
                    size_t, ucp_tag_t, const ucp_request_param_t*);
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
    [](ucp_tag_message_h found, ucp_worker_h, ucp_tag_t, ucp_tag_t, int, ucp_tag_recv_info_t*) {
        return found != nullptr;
    },
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

// The local worker's address, and the only door that offers it. Hooked for
// identity rather than for traffic: with this and UcxEpConnection below, an
// endpoint in rank 0's capture can be matched to the worker that created it in
// rank 3's, which is the one join in the UCX records that crosses a rank.
//
// SigmaTrace's ucx-endpoint branch does the same thing and hashes with
// absl::HashOf. We use FNV-1a instead, and the difference is not stylistic:
// absl's hashes are not guaranteed stable across processes, and a per-process
// salt would make every cross-rank join silently miss while looking perfectly
// well-formed. A join key that travels between captures has to be deterministic.
DEFINE_PRELOAD_HOOK(ucx, worker_get_address, "ucp_worker_get_address", ucs_status_t, ucp_worker_h,
                    ucp_address_t**, size_t*);

// THE SAME FACT, THROUGH THE DOOR THAT IS ACTUALLY USED. `ucp_worker_get_address`
// above is not how an MPI application obtains its worker address. OpenMPI's
// `mca_pml_ucx` — the component beneath every MPI_Send, MPI_Alltoall and halo
// exchange — calls `ucp_worker_query` with UCP_WORKER_ATTR_FIELD_ADDRESS and
// never calls the older entry point; only the one-sided `mca_osc_ucx` and
// `mca_spml_ucx` use that one.
//
// So before this hook existed, every OpenMPI point-to-point application produced
// a `ucx_worker_address` row for the wrong one of its two workers — the
// one-sided worker, which carries no traffic — and
// `ucx_ep_connection.remote_prefix_hash` therefore pointed at nothing. That
// field is what ucx_records.hpp calls "the only thing in the UCX records that
// crosses a rank boundary", so the practical effect was that the provider could
// not produce a communication matrix for MPI p2p at all.
//
// Found independently by three of the four application studies in
// tests/hpc/apps/ (GROMACS, LAMMPS, Quantum ESPRESSO), from three different
// angles, and by none of the workloads we wrote ourselves — every one of those
// reaches UCX through a door we already knew about. This is the same shape as
// case 08's empty ib_queue_pair: an API hooked in one of its two spellings.
DEFINE_PRELOAD_HOOK(ucx, worker_query, "ucp_worker_query", ucs_status_t, ucp_worker_h,
                    ucp_worker_attr_t*);

// THE REMOTE KEYS, which every one-sided row already named and nothing created.
//
// `ucx_rma.rkey` and `ucx_atomic.rkey` intern a `ucp_rkey_h` that no record
// opened, so the handle joined to nothing — an entity referenced by events that
// nothing creates, which is the defect the verbs provider spent a week on.
// UcxRkey closes it and gives the handle a lifetime, without which a reallocated
// rkey pointer reads as the one before it.
//
// Hooked on OBSERVED consumers rather than on a static reference this time. On
// MN5's HPC-X 2.22.1 the undefined-symbol survey gives:
//
//   mca_osc_ucx    MPI one-sided   ucp_ep_rkey_unpack, ucp_rkey_pack,
//                                  ucp_rkey_destroy, ucp_rkey_buffer_release
//   mca_spml_ucx   OpenSHMEM       the same four, plus ucp_rkey_compare
//   libnccl-net    NCCL plugin     the same four
//   mca_pml_ucx    MPI p2p         none — point-to-point unpacks no keys
//
// and `mca_osc_ucx` additionally needs `ucp_put_nbi`, `ucp_get_nbi`,
// `ucp_atomic_post` and `ucp_atomic_fetch_nb`, all already hooked. So an MPI RMA
// workload writes `ucx_rma` rows through doors we watch, naming keys we did not
// record — which is exactly the hole these three close.
//
// `ucp_rkey_buffer_release` is NOT hooked, and that is a correction to this
// plan's own earlier sketch rather than an omission. It frees the packed BUFFER
// produced by `ucp_rkey_pack`; it does not destroy a key. UcxRkeyPack is an
// Artifact with no lifetime, so there is nothing for it to close, and emitting a
// close for it would put a close in front of a reader for an object that never
// opened. `ucp_memh_pack` / `ucp_memh_buffer_release` — the modern replacements
// — are exported by libucp and called by nothing on this stack, so they are a
// deliberate omission, recorded here so the next survey does not re-derive it.
DEFINE_PRELOAD_HOOK(ucx, ep_rkey_unpack, "ucp_ep_rkey_unpack", ucs_status_t, ucp_ep_h, const void*,
                    ucp_rkey_h*);
DEFINE_PRELOAD_HOOK(ucx, rkey_pack, "ucp_rkey_pack", ucs_status_t, ucp_context_h, ucp_mem_h, void**,
                    size_t*);
DEFINE_PRELOAD_HOOK(ucx, rkey_destroy, "ucp_rkey_destroy", void, ucp_rkey_h);

// THE RENDEZVOUS DOOR, and the first NON-PUBLIC symbol this project interposes.
//
// The three hooks above left a hole that case 21 measured: job 45198862 recorded
// 4 rkeys per rank and 43 closes, because `ucp_rkey_destroy` is public and fires
// for keys created somewhere we were not watching. That somewhere is
// `ucp_ep_rkey_unpack_internal`, which UCX's rendezvous protocol calls directly
// (src/ucp/rndv/proto_rndv.c) — about two per rendezvous, on both ranks, and
// zero in case 22 which does one-sided RMA and no large rendezvous.
//
// INTERPOSABLE, verified on the shipped library rather than assumed. libucp.so
// carries no DT_SYMBOLIC and no BIND_NOW, the symbol is GLOBAL DEFAULT, and both
// of its intra-library call sites go through the PLT:
//   objdump: 2 × `call ucp_ep_rkey_unpack_internal@plt`, 0 direct calls
// A library built -Bsymbolic-functions would bind those internally and this hook
// would silently never fire, so that check is the precondition, not a detail.
//
// SEVEN PARAMETERS DECLARED FOR A FUNCTION THAT HAS SIX ON THIS MACHINE, and
// that is the whole trick. The signature gained `ucs_sys_device_t sys_device`
// immediately before the out-parameter between two releases — pinned by reading
// both tags, not inferred:
//
//   v1.18.0  (ep, buffer, length, unpack_md_map, skip_md_map,             rkey_p)
//   v1.19.0  (ep, buffer, length, unpack_md_map, skip_md_map, sys_device, rkey_p)
//
// so `rkey_p` is argument 6 on the HPC-X 2.22.1 build MN5 runs and argument 7
// from 1.19 on — %r9 versus the first stack slot. Declaring the superset and
// choosing at RUNTIME is the only correct option here, because this .so is built
// once and dlopened against whatever libucp the application already loaded; a
// build-time #if would bake in the wrong answer for every other cluster.
//
// Under the SysV AMD64 ABI both directions of the mismatch are safe. Forwarding
// seven integer/pointer arguments to a six-parameter callee is fine — the callee
// ignores the extra one and the caller cleans up. Reading the seventh when only
// six were passed yields an undefined stack slot, which is why the provider
// reads it ONLY when the version says it is real.
//
// The trailing slots are typed as plain uint64_t rather than with UCX typedefs
// on purpose: `ucs_sys_device_t` is an enum whose width is not part of any
// contract, and this hook must not stop compiling when it changes.
DEFINE_PRELOAD_HOOK(ucx, ep_rkey_unpack_internal, "ucp_ep_rkey_unpack_internal", ucs_status_t,
                    ucp_ep_h, const void*, size_t, std::uint64_t, std::uint64_t, std::uint64_t,
                    std::uint64_t);
