#pragma once
// NCCL hook declarations.
//
// Each hook RESOLVES THE SAME NAME IT EXPORTS, like every other provider here.
//
// It first resolved the `p`-prefixed twin instead, which NCCL provides: every
// public entry point is declared through the NCCL_API macro
// (reference/nccl-git/src/include/core.h:18), which under PROFAPI emits
//
//     __attribute__((alias("ncclX"))) pncclX;   // strong
//     __attribute__((weak))           ncclX;    // weak
//
// — the PMPI construction. Resolving `pncclX` looks strictly better on paper: a
// distinct strong symbol at the same address, which cannot loop back into our
// own interposer.
//
// IT ABORTS EVERY RANK, and the reason is worth keeping. register_real
// (detail/PreloadHookManager.hpp) finds its OWN base address by looking up
// `Trait::symbol_name` through RTLD_DEFAULT and calling dladdr on the result —
// that is how it rejects a resolution that loops back to the provider. The
// mechanism assumes the hook exports the name it resolves. Ours did not: the
// RTLD_DEFAULT lookup of `pncclCommInitRank` landed in libnccl, so `our_base`
// became LIBNCCL's base, every subsequent resolution was classified as
// self-resolution and thrown away, and scan_loaded_libraries then skipped the
// one library that had the symbol. Measured on job 44723546: "[hook] Failed to
// register real function for pncclCommInitRank", SIGABRT on all four ranks,
// with the backtrace entering through our own ncclCommInitRank.
//
// So the plain name it is, and the weak/strong fact still does real work — it
// is what makes our definition win the symbol for the whole process regardless
// of load order, where an ordinary LD_PRELOAD interposer is relying on the
// loader's search order to break a tie between two strong definitions.
//
// The p-twins are still used, just not for hook resolution: the provider CALLS
// pncclCommUserRank, pncclGroupStart and friends directly (see
// nccl_provider.cpp). Those are ordinary undefined references the loader binds
// to libnccl, and since we export no symbol by those names there is no tie and
// no guard to confuse.
//
// PROFAPI is NCCL's default and has been for its whole public life. Verified on
// every NCCL on MN5: 2.26.5, 2.29.3, and the /apps/ACC modules 2.19.4, 2.20.5,
// 2.24.3-1.

#include <cuda_runtime_api.h>
#include <nccl.h>

#include "comshu/hook/HookTrait.hpp"

// --- communicator lifecycle -------------------------------------------------
// ncclCommInitRank carries the ncclUniqueId BY VALUE, which is the only place
// the cross-rank identity of a communicator is visible to us: it is an input,
// so it cannot be recovered from the comm afterwards through any public call.
DEFINE_PRELOAD_HOOK(nccl, comm_init_rank, "ncclCommInitRank", ncclResult_t, ncclComm_t*, int,
                    ncclUniqueId, int);
DEFINE_PRELOAD_HOOK(nccl, comm_init_rank_config, "ncclCommInitRankConfig", ncclResult_t,
                    ncclComm_t*, int, ncclUniqueId, int, ncclConfig_t*);
DEFINE_PRELOAD_HOOK(nccl, comm_destroy, "ncclCommDestroy", ncclResult_t, ncclComm_t);
// Abort is a destruction door like destroy, and a workload that dies through it
// would otherwise leave every communicator looking un-closed —
// entity_close_accounted's third clause, firing on a legal teardown path.
DEFINE_PRELOAD_HOOK(nccl, comm_abort, "ncclCommAbort", ncclResult_t, ncclComm_t);

// --- collectives ------------------------------------------------------------
DEFINE_PRELOAD_HOOK(nccl, all_reduce, "ncclAllReduce", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, ncclRedOp_t, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, broadcast, "ncclBroadcast", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, int, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, reduce, "ncclReduce", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, ncclRedOp_t, int, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, all_gather, "ncclAllGather", ncclResult_t, const void*, void*, size_t,
                    ncclDataType_t, ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, reduce_scatter, "ncclReduceScatter", ncclResult_t, const void*, void*,
                    size_t, ncclDataType_t, ncclRedOp_t, ncclComm_t, cudaStream_t);

// --- point to point ---------------------------------------------------------
// The only calls that name a peer, and therefore the only ones from which an
// exact edge follows without modelling a collective's internal algorithm.
DEFINE_PRELOAD_HOOK(nccl, send, "ncclSend", ncclResult_t, const void*, size_t, ncclDataType_t, int,
                    ncclComm_t, cudaStream_t);
DEFINE_PRELOAD_HOOK(nccl, recv, "ncclRecv", ncclResult_t, void*, size_t, ncclDataType_t, int,
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
#define COMSHU_NCCL_HAS_WINDOW 1
DEFINE_PRELOAD_HOOK(nccl, window_register, "ncclCommWindowRegister", ncclResult_t, ncclComm_t,
                    void*, size_t, ncclWindow_t*, int);
DEFINE_PRELOAD_HOOK(nccl, window_deregister, "ncclCommWindowDeregister", ncclResult_t, ncclComm_t,
                    ncclWindow_t);
// ncclDevCommCreate is declared in nccl_device/core.h, which is host-includable
// — but its requirements argument is taken as an opaque pointer here rather
// than by its real type. That struct leads with `size`, `magic` and `version`
// fields, which is NVIDIA saying outright that its layout is expected to
// change; a provider that reached into it would misread an int after an NCCL
// upgrade instead of failing. The fact worth recording is that a device
// communicator was created at all, and the pointer alone carries that.
DEFINE_PRELOAD_HOOK(nccl, dev_comm_create, "ncclDevCommCreate", ncclResult_t, ncclComm_t,
                    const void*, void*);
#else
#define COMSHU_NCCL_HAS_WINDOW 0
#endif
