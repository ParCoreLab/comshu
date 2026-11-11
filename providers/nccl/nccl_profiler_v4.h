#pragma once
/* NCCL profiler plugin ABI, version 4 — vendored.
 *
 * WHY VENDORED AND NOT FOUND. These declarations live in NCCL's SOURCE tree
 * (src/include/plugin/nccl_profiler.h and the plugin example's profiler_v4.h),
 * not in the headers an NCCL install ships: $NCCL_HOME/include has nccl.h and
 * nccl_device.h and nothing about profiling. A find_path() for them would
 * succeed on a developer box with a checkout and fail on every cluster, which
 * is the worst of both worlds — a provider that silently is not built. Copied
 * from reference/nccl-git/plugins/profiler/example/nccl/{common.h,profiler_v4.h}
 * and reference/nccl-git/src/include/plugin/nccl_profiler.h.
 *
 * WHY v4 AND ONLY v4, WHICH IS NOT THE OBVIOUS CHOICE. NCCL resolves the newest
 * plugin symbol it knows and works downwards
 * (reference/nccl-git/src/plugin/profiler.cc:73-89: v6, then v5, v4, v3 ...), so
 * the instinct is to target the newest. What settles it is which libnccl the
 * CASES actually load, measured on each machine rather than read off a version
 * table:
 *
 *   MN5   $NCCL_HOME -> HPC SDK 26.5, comm_libs/12.9/nccl   2.29.3   v1..v6
 *   du04  $NCCL_HOME -> HPC SDK 25.9, comm_libs/13.0/nccl            v1..v4
 *
 * v4 is the newest present in BOTH. It is native on du04 and reached through
 * NCCL's own down-shim on MN5. One struct, one code path, identical behaviour
 * everywhere we can run.
 *
 * This is worth the paragraph because the first version of this file targeted
 * v5, chosen from du04's /lib/x86_64-linux-gnu/libnccl.so.2 (2.28.9, v1..v5).
 * That is the SYSTEM library and it is not what the cases load — $NCCL_HOME
 * points into the HPC SDK. A v5 plugin would have exported a symbol du04's NCCL
 * never looks for, and the failure mode is silence: NCCL logs "is unsupported"
 * at INFO level and runs on, leaving an empty table that looks like a workload
 * which did no collectives.
 *
 * WHAT v4 COSTS, checked rather than assumed. On NCCL >= 2.28 the down-shim
 * (reference/nccl-git/src/plugin/profiler/profiler_v4.cc:24-38) forwards every
 * coll field this plugin reads — seqNumber, func, count, root, datatype,
 * nChannels, nWarps, algo, proto — and sets parentObj to the plan's group event.
 * proxyOp is forwarded whole. Nothing we read is dropped. Contrast v3, whose
 * shim assigns coll.commHash = 0 and coll.name = nullptr outright
 * (profiler_v3.cc:29-30, commented "removed in v4"): a v3 plugin on a modern
 * NCCL emits rows naming no communicator. What v4 genuinely lacks is the
 * groupApi/collApi/p2pApi/kernelLaunch families v5 added, which this plugin does
 * not enable.
 *
 * THIS FILE IS AN ABI CONTRACT. Field order and types are NCCL's; a mismatch
 * does not fail to compile, it reads the wrong bytes at runtime. Do not reorder,
 * do not "tidy" the unused union arms away — the union's size and the offsets of
 * the arms we do read depend on all of them.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The entry points below return `int` where NCCL declares ncclResult_t. That is
 * an unscoped enum with small values, so it is passed in the same register and
 * the two are ABI-identical; spelling it `int` is what lets this header stand
 * alone without nccl.h. 0 is ncclSuccess and is the only value we ever return —
 * a profiler plugin that fails an entry point gets unloaded by NCCL
 * (reference/nccl-git/src/plugin/profiler.cc:255-258), taking the rest of the
 * capture's profiler rows with it, so this door never reports failure upward. */

typedef enum {
    NCCL_LOG_NONE = 0,
    NCCL_LOG_VERSION = 1,
    NCCL_LOG_WARN = 2,
    NCCL_LOG_INFO = 3,
    NCCL_LOG_ABORT = 4,
    NCCL_LOG_TRACE = 5
} comshuNcclDebugLogLevel;

typedef void (*comshuNcclDebugLogger_t)(comshuNcclDebugLogLevel level, unsigned long flags,
                                       const char* file, int line, const char* fmt, ...);

/* Event type bitmask. Doubles as the activation mask the plugin writes back at
 * init: NCCL only generates the families we ask for, so this is the one lever
 * that controls how much this door costs. Bits 8+ are v5 families and have no
 * meaning to a v4 plugin.
 * (reference/nccl-git/src/include/plugin/nccl_profiler.h) */
enum {
    comshuNcclProfileGroup = (1 << 0),
    comshuNcclProfileColl = (1 << 1),
    comshuNcclProfileP2p = (1 << 2),
    comshuNcclProfileProxyOp = (1 << 3),
    comshuNcclProfileProxyStep = (1 << 4),
    comshuNcclProfileProxyCtrl = (1 << 5),
    comshuNcclProfileKernelCh = (1 << 6),
    comshuNcclProfileNetPlugin = (1 << 7),
};

/* v4 event descriptor. VERBATIM layout from
 * reference/nccl-git/plugins/profiler/example/nccl/profiler_v4.h. Note `type` is
 * uint8_t here; v5 widened it to uint64_t. */
typedef struct {
    uint8_t type;
    void*   parentObj;
    int     rank;
    union {
        struct {
            uint64_t    seqNumber;
            const char* func;
            void const* sendBuff;
            void*       recvBuff;
            size_t      count;
            int         root;
            const char* datatype;
            uint8_t     nChannels;
            uint8_t     nWarps;
            const char* algo;
            const char* proto;
        } coll;

        struct {
            const char* func;
            void*       buff;
            const char* datatype;
            size_t      count;
            int         peer;
            uint8_t     nChannels;
        } p2p;

        struct {
            pid_t   pid;
            uint8_t channelId;
            int     peer;
            int     nSteps;
            int     chunkSize;
            int     isSend;
        } proxyOp;

        struct {
            int step;
        } proxyStep;

        struct {
            uint8_t  channelId;
            uint64_t pTimer;
        } kernelCh;

        struct {
            int64_t id;
            void*   data;
        } netPlugin;
    };
} comshuNcclProfilerEventDescr_v4_t;

typedef union {
    struct {
        size_t transSize;
    } proxyStep;

    struct {
        int appendedProxyOps;
    } proxyCtrl;

    struct {
        void* data;
    } netPlugin;

    struct {
        uint64_t pTimer;
    } kernelCh;
} comshuNcclProfilerEventStateArgs_v4_t;

/* The vtable NCCL dlsym's as "ncclProfiler_v4".
 *
 * `commHash` is comm->commHash (reference/nccl-git/src/plugin/profiler.cc:250) —
 * see nccl_comm_hash.hpp for why that is the join key and how we recompute it on
 * the host-API side. NOTE the argument order: v4 takes (context, mask, commName,
 * commHash, ...) while v5 reordered it to (context, commId, mask, commName, ...).
 * The two are not interchangeable and the compiler cannot tell them apart, since
 * both are pointer-then-scalar soups. */
typedef struct {
    const char* name;
    int (*init)(void** context, int* eActivationMask, const char* commName, uint64_t commHash,
                int nNodes, int nranks, int rank, comshuNcclDebugLogger_t logfn);
    int (*startEvent)(void* context, void** eHandle, comshuNcclProfilerEventDescr_v4_t* eDescr);
    int (*stopEvent)(void* eHandle);
    int (*recordEventState)(void* eHandle, int eState,
                            comshuNcclProfilerEventStateArgs_v4_t* eStateArgs);
    int (*finalize)(void* context);
} comshuNcclProfiler_v4_t;

#ifdef __cplusplus
}
#endif
