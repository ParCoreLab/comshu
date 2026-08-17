#pragma once
// NCCL hook declarations.
//
// EVERY SYMBOL HERE RESOLVES TO ITS `p`-PREFIXED TWIN, and that is the reason
// this provider is the least fragile one in the tree.
//
// NCCL declares every public entry point through the NCCL_API macro
// (reference/nccl-git/src/include/core.h:18), which under PROFAPI emits
//
//     __attribute__((alias("ncclX"))) pncclX;   // strong
//     __attribute__((weak))           ncclX;    // weak
//
// — the PMPI construction. Two consequences, both of which we want:
//
//   1. Our `ncclX` is STRONG, so it wins the symbol against libnccl's weak one
//      no matter the load order. No LD_PRELOAD ordering to get right, and no
//      repeat of the dlvsym hunt in PLAN-20260806-2320.
//   2. `pncclX` is a distinct strong symbol at the same address, so resolving
//      it can never loop back into our own interposer — which is the failure
//      mode RTLD_NEXT exists to dodge, dodged here by construction instead.
//
// Verified on every NCCL on MN5: the HPC SDK 26.5's 2.29.3 (what the cases
// load, 63 pnccl symbols), plus the /apps/ACC modules 2.19.4, 2.20.5 and
// 2.24.3-1. PROFAPI is NCCL's default and has been for its whole public life.
//
// IF A BUILD EVER SHIPS WITHOUT IT, the pnccl symbol is absent, resolution
// fails, and the hook reports as unresolved rather than silently recording
// nothing — see CompositeHookManager::invoke_preload, which traces the entry
// before it resolves for exactly this reason.

#include <cuda_runtime_api.h>
#include <nccl.h>

#include "pcomm/hook/HookTrait.hpp"

// --- communicator lifecycle -------------------------------------------------
// ncclCommInitRank carries the ncclUniqueId BY VALUE, which is the only place
// the cross-rank identity of a communicator is visible to us: it is an input,
// so it cannot be recovered from the comm afterwards through any public call.
DEFINE_PRELOAD_HOOK(nccl, comm_init_rank, "pncclCommInitRank", ncclResult_t, ncclComm_t*, int,
                    ncclUniqueId, int);
DEFINE_PRELOAD_HOOK(nccl, comm_init_rank_config, "pncclCommInitRankConfig", ncclResult_t,
                    ncclComm_t*, int, ncclUniqueId, int, ncclConfig_t*);
DEFINE_PRELOAD_HOOK(nccl, comm_destroy, "pncclCommDestroy", ncclResult_t, ncclComm_t);
// Abort is a destruction door like destroy, and a workload that dies through it
// would otherwise leave every communicator looking un-closed —
// entity_close_accounted's third clause, firing on a legal teardown path.
DEFINE_PRELOAD_HOOK(nccl, comm_abort, "pncclCommAbort", ncclResult_t, ncclComm_t);

// --- collectives ------------------------------------------------------------
DEFINE_PRELOAD_HOOK(nccl, all_reduce, "pncclAllReduce", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, ncclRedOp_t, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, broadcast, "pncclBroadcast", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, int, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, reduce, "pncclReduce", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, ncclRedOp_t, int, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, all_gather, "pncclAllGather", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, reduce_scatter, "pncclReduceScatter", ncclResult_t, const void*, void*,
                    size_t, ncclDataType_t, ncclRedOp_t, ncclComm_t, cudaStream_t);

// --- point to point ---------------------------------------------------------
// The only calls that name a peer, and therefore the only ones from which an
// exact edge follows without modelling a collective's internal algorithm.
DEFINE_PRELOAD_HOOK(nccl, send, "pncclSend", ncclResult_t, const void*, size_t, ncclDataType_t, int,
                    ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, recv, "pncclRecv", ncclResult_t, void*, size_t, ncclDataType_t, int,
                    ncclComm_t, cudaStream_t);

// --- grouping ---------------------------------------------------------------
// Without these, N grouped sends read as N launches when they are one.
//
// NOT declared here, and not through DEFINE_PRELOAD_HOOK at all: ncclGroupStart
// and ncclGroupEnd take NO arguments, and the macro expands its parameter pack
// into `HookSigs<ret_type, __VA_ARGS__>` — which with an empty pack is
// `HookSigs<ncclResult_t, >`, a syntax error. Teaching the macro about the
// nullary case means editing machinery every other provider depends on, to
// serve two symbols. These two are interposed by hand in nccl_provider.cpp
// instead, calling pncclGroupStart/pncclGroupEnd directly — which is what the
// generated path would have done anyway, since the resolution here is a plain
// link-time reference to the p-twin rather than a dlsym.

// --- symmetric memory, the host half of the device-initiated path -----------
// Present since NCCL 2.27-ish. Guarded so this provider still builds against an
// older header; the hook simply does not exist there, and a capture from such a
// build has no nccl_window table rather than an empty one.
#if defined(NCCL_MAJOR) && (NCCL_MAJOR > 2 || (NCCL_MAJOR == 2 && NCCL_MINOR >= 27))
#define PCOMM_NCCL_HAS_WINDOW 1
DEFINE_PRELOAD_HOOK(nccl, window_register, "pncclCommWindowRegister", ncclResult_t, ncclComm_t,
                    void*, size_t, ncclWindow_t*, int);
DEFINE_PRELOAD_HOOK(nccl, window_deregister, "pncclCommWindowDeregister", ncclResult_t, ncclComm_t,
                    ncclWindow_t);
// ncclDevCommCreate is declared in nccl_device/core.h, which is host-includable
// — but its requirements argument is taken as an opaque pointer here rather
// than by its real type. That struct leads with `size`, `magic` and `version`
// fields, which is NVIDIA saying outright that its layout is expected to
// change; a provider that reached into it would misread an int after an NCCL
// upgrade instead of failing. The fact worth recording is that a device
// communicator was created at all, and the pointer alone carries that.
DEFINE_PRELOAD_HOOK(nccl, dev_comm_create, "pncclDevCommCreate", ncclResult_t, ncclComm_t,
                    const void*, void*);
#else
#define PCOMM_NCCL_HAS_WINDOW 0
#endif
