#pragma once
// The HOST doorbell path — option (b) of
// PLAN-20260905-1224-cpu-assisted-ibgda-is-the-cluster-we-have.
//
// The GPU doorbell hijack (uar_hijack.hpp) observes a device-initiated send by
// swapping the device pointer the GPU rings. That mechanism needs the NIC's BAR
// page mapped into the GPU, which many production clusters do not permit — the
// open kernel module has no PeerMappingOverride and cuMemHostRegister on the UAR
// returns cudaErrorNotPermitted. MN5 is one of them. NVSHMEM then runs
// CPU-assisted IBGDA: the GPU still builds every WQE in a GPU-resident send
// queue and advances a producer index, but a HOST thread rings the doorbell
// (ibgda_rc_progress / ibgda_dci_progress in the transport). There is no GPU
// store to intercept, and mlx5_device_send is empty for a platform reason.
//
// This is the door for that mode, and it is a smaller mechanism than the GPU
// one: the doorbell is an ordinary host store to an ordinary host mapping, so
// the swap is a pointer in a struct and the forward is a store on the same
// side of PCIe. No fake device page, no cuMemHostRegister, no CUDA at all on
// the doorbell path itself — only the WQE copy-out still needs the driver,
// because the send queue is still in GPU memory.
//
// WHY MUTATING reg_addr IN PLACE IS SAFE, which is a claim about somebody
// else's struct and so was read rather than assumed
// (reference/rdma-core/providers/mlx5/verbs.c:6548-6632):
//
//   * The returned `mlx5dv_devx_uar*` is EMBEDDED in libmlx5's `struct mlx5_bf`,
//     and `_mlx5dv_devx_free_uar` recovers the owner with
//     `container_of(dv_devx_uar, struct mlx5_bf, devx_uar.dv_devx_uar)`. So the
//     pointer we return must be the real one — handing back a substitute struct
//     would compute a garbage mlx5_bf and corrupt the free. We therefore mutate
//     the field, not the pointer.
//   * `reg_addr` is a COPY, written once at alloc from `bf->reg`. The free path
//     uses `bf->reg` and `bf->uar` and never reads the public copy back, so
//     changing it cannot disturb teardown.
//   * `MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC` returns a per-context SINGLETON whose
//     free is a no-op, so one pointer can come back from several alloc calls.
//     Arming is therefore refcounted per `mlx5dv_devx_uar*` rather than assumed
//     one-to-one. (NVSHMEM asks for NC_DEDICATED, which is not a singleton, but
//     which of the two a stack asks for is not ours to depend on.)
//
// MUTUALLY EXCLUSIVE WITH THE GPU HIJACK, and not merely by convention. If this
// path swapped reg_addr while a stack was still probing for GPU submission, the
// transport would call cuMemHostRegister on ORDINARY HOST MEMORY, which can
// succeed where the real BAR page cannot — and NVSHMEM would then select its GPU
// handler on a platform that cannot do GPU submission. That is not observing a
// run, it is changing which code path the application takes, and it is the
// "never manufacture a capability" rule of uar_hijack.hpp with the sign flipped.
// So the two are chosen explicitly, one per run, and asking for both disables
// this one.

#include <atomic>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <sys/mman.h>

#include <infiniband/mlx5dv.h>

#include "memory_repeater.hpp"
#include "uar_hijack.hpp"

namespace mlx5::hostdb {

inline constexpr std::size_t kUarPageSize = 4096;

// The launcher's --enable-host-doorbell sets COMSHU_HOST_DOORBELL=1. Checked
// once, and refused outright when the GPU hijack is also asked for: a run gets
// one doorbell mechanism, and which one is a property of the platform, not a
// preference.
inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_HOST_DOORBELL");
        const bool  want = v && *v == '1';
        if (want && hijack::enabled()) {
            std::fprintf(stderr,
                         "[comshu] COMSHU_HOST_DOORBELL and COMSHU_DOORBELL_HIJACK are both set. "
                         "They observe the same doorbell by opposite means and cannot share a "
                         "run; the host path is DISABLED and the GPU hijack stands. Pick one: "
                         "the GPU hijack needs the NIC BAR mapped into the GPU, the host path "
                         "is for the CPU-assisted mode that runs where it is not.\n");
            return false;
        }
        return want;
    }();
    return on;
}

struct Armed {
    void*         decoy{nullptr};  // the page the application now rings
    void*         real{nullptr};   // the doorbell register it used to ring
    unsigned      refs{0};         // alloc_uar can hand back one singleton twice
    std::vector<repeater::RepeaterHandle> handles;
};

struct State {
    std::mutex                                mutex;
    std::unordered_map<void*, Armed>          armed;  // mlx5dv_devx_uar* -> swap
    std::atomic<std::uint32_t>                uars_armed{0};
    std::atomic<std::uint32_t>                uars_unarmed{0};  // failed open
};

// One-shot flags so an inconclusive probe reports once rather than once per UAR
// (a run can allocate a hundred).
inline bool g_said_unresolved = false;
inline bool g_said_inconclusive = false;

inline State& state() {
    static State s;
    return s;
}

inline bool debug_on() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_HOST_DOORBELL_DEBUG");
        return v && *v == '1';
    }();
    return on;
}

// Can this platform map a NIC BAR page into a GPU at all? Asked ONCE, of the
// driver, using the exact call the platform gates on — and it is the guard that
// makes this whole mechanism safe rather than merely documented.
//
// The hazard: a transport reads `uar->reg_addr` and immediately registers it
// with CUDA (NVSHMEM's ibgda_alloc_and_map_qp_uar does both in one function),
// so by the time anything else could intervene the decoy is already what it
// registered. Swapping cannot be deferred past that point either — NVSHMEM
// copies the pointer at allocation, so a later mutation would never be seen.
// On a platform that CAN do GPU submission, arming would therefore hand the
// transport ordinary host memory where it expected I/O memory, and change which
// submission mode it selects. Observing a run must never do that.
//
// So the swap is allowed only where GPU submission is already impossible, and
// that is established by measurement, not by the user's choice of flag: we
// register the real UAR page exactly as the transport would and look at what the
// driver says. cudaErrorNotPermitted (the open kernel module with no
// PeerMappingOverride) means the transport's own attempt will fail identically,
// so arming cannot change its mind about anything.
//
// FAILS CLOSED, unlike everything else here, and deliberately: an inability to
// prove GPU mapping impossible leaves the UAR untouched. The cost of being
// wrong in this direction is an empty table; in the other, it is a run that
// took a different code path because it was being watched.
inline bool platform_refuses_gpu_doorbell(void* real_reg_addr) {
    // Only a DEFINITIVE answer is cached. A probe that could not run is not
    // evidence about the platform, and caching it as one is what made the first
    // real case-12 run leave all 96 UARs unarmed: the transport allocates its
    // UARs before it has a current CUDA context, cuMemHostRegister returned
    // CUDA_ERROR_INVALID_CONTEXT (201) on the very first one, and every UAR
    // after it inherited that non-answer. Retrying costs one driver call per
    // UAR until the question becomes answerable, which it does as soon as the
    // application has a context.
    static int cached = -1;  // -1 not yet answered, 0 mapping works, 1 refused
    if (cached >= 0)
        return cached == 1;

    export_table::promote_libcuda();
    auto* reg = reinterpret_cast<hijack::PfnHostRegister>(
        export_table::real("cuMemHostRegister_v2"));
    auto* unreg = reinterpret_cast<hijack::PfnHostUnregister>(
        export_table::real("cuMemHostUnregister"));
    if (!reg || !unreg) {
        // Not cached: libcuda may simply not be loaded yet.
        if (!std::exchange(g_said_unresolved, true))
            std::fprintf(stderr,
                         "[comshu] host doorbell: cannot resolve the CUDA driver yet, so whether "
                         "this platform maps a NIC doorbell page into the GPU is unknown. NOT "
                         "arming this UAR; will ask again on the next one.\n");
        return false;
    }

    // The SAME call the transport makes — address, size and flags — because a
    // probe that differs anywhere answers a different question. NVSHMEM's
    // ibgda_nic_mem_gpu_map does:
    //
    //     cudaHostRegister(uar->reg_addr, IBGDA_MLX5_NC_UAR_SIZE /* 8 */,
    //                      Portable | Mapped | IoMemory)
    //
    // Eight bytes at the UNALIGNED reg_addr, not a page at its base. Both
    // details were learned by getting them wrong on du04, a platform that does
    // support GPU submission: a 4096-byte flagless probe there returned
    // CUDA_ERROR_INVALID_VALUE, which the first version of this guard read as
    // "the driver refused" and armed on — the exact outcome the guard exists to
    // prevent.
    const int rc = reg(real_reg_addr, hijack::kDoorbellRegBytes,
                       hijack::kMemHostRegisterPortable | hijack::kMemHostRegisterDeviceMap |
                           hijack::kMemHostRegisterIoMemory);
    if (rc == hijack::kCudaSuccess) {
        unreg(real_reg_addr);  // leave the page exactly as it was found
        std::fprintf(stderr,
                     "[comshu] host doorbell: this platform DOES map the NIC doorbell page into "
                     "the GPU, so its transport can submit from the device and arming the host "
                     "path would change which mode it picks. NOT arming — use "
                     "--enable-doorbell-hijack here.\n");
        cached = 0;
        return false;
    }

    // Only ONE error proves the platform refuses: CUDA_ERROR_NOT_PERMITTED, the
    // open kernel module without PeerMappingOverride. Every other failure is a
    // failure of this probe — wrong flags, wrong context, a page that is not the
    // one the transport will register — and must not be read as evidence about
    // the platform. Fail closed.
    if (rc != hijack::kCudaErrorNotPermitted) {
        // Also not cached, and for the same reason: this says the probe could
        // not be made, not that the platform answered. CUDA_ERROR_INVALID_CONTEXT
        // (201) is the common one — no context is current on the thread that
        // allocated this UAR — and it resolves itself once the application makes
        // one, so the next UAR gets a real answer.
        if (!std::exchange(g_said_inconclusive, true))
            std::fprintf(stderr,
                         "[comshu] host doorbell: probing the doorbell page failed with rc=%d, "
                         "which is not the refusal this guard looks for (%d, NOT_PERMITTED) — "
                         "201 means no CUDA context was current yet. That says the probe could "
                         "not be made, not that the platform refuses. NOT arming this UAR; will "
                         "ask again on the next one.\n",
                         rc, hijack::kCudaErrorNotPermitted);
        return false;
    }

    std::fprintf(stderr,
                 "[comshu] host doorbell: the driver refuses to map the NIC doorbell page into "
                 "the GPU (cuMemHostRegister -> NOT_PERMITTED). GPU-submitted IBGDA is "
                 "impossible on this platform, so the doorbell writer is a host thread and "
                 "watching its mapping cannot change the run.\n");
    cached = 1;
    return true;
}

// mlx5dv_devx_alloc_uar allocation types. Spelled out rather than included,
// because the header that names the third one is not everywhere: du04's
// rdma-core 52 has only BF and NC, MN5's DOCA OFED adds
// `#define MLX5DV_UAR_ALLOC_TYPE_NC_DEDICATED (1U << 31)` in
// infiniband/mlx5_api.h. Compiling against the older header must not silently
// turn the test below into something else.
inline constexpr std::uint32_t kUarAllocBf = 0x0;
inline constexpr std::uint32_t kUarAllocNc = 0x1;
inline constexpr std::uint32_t kUarAllocNcDedicated = 0x80000000u;

// IS THIS UAR ONE OF OURS TO WATCH?
//
// It has to be answered HERE, at allocation, because the transport copies
// reg_addr the moment it gets it — the header above says so, and moving the arm
// to CREATE_QP to use a better signal produced an empty capture and a red case
// 12 (job 45587885). Allocation flags are the only thing known this early.
//
// They are enough, measured rather than assumed (job 45588970, two nodes,
// passive so nothing was armed). 192 UARs were allocated in one rank:
//
//   flags=0x0          180 UARs, 17 distinct pages   none rung by a GPU-driven QP
//   flags=0x1            6 UARs                      none rung by a GPU-driven QP
//   flags=0x80000000     6 UARs, page 280            ALL SIX GPU-driven QPs
//
// So BF (0x0) is everybody else — UCX's RC transport above all — and arming it
// is what broke MPI: the repeater forwards eight bytes from offset 0 while
// BlueFlame writes whole 64-byte WQEs into alternating buffers in the page, so
// UCX's sends stopped reaching the NIC and a two-node run hung inside
// MPI_Comm_dup in NVSHMEM's own bootstrap.
//
// NC and NC_DEDICATED are what IBGDA asks for, in that preference order —
// ibgda.cpp's ibgda_create_cq_shared_objects tries NC_DEDICATED, falls back to
// NC, and only then to BF. Both non-BF types are admitted because which one a
// run gets depends on the rdma-core it links against, and a capture should not
// depend on that.
//
// THE BF FALLBACK IS DELIBERATELY NOT COVERED. If IBGDA gets neither NC type it
// allocates BF, and this will decline to arm it — the run captures no device
// sends. That is the correct direction to be wrong in: the alternative is
// arming every BF UAR in the process to catch one, which is precisely the bug
// being fixed. A capture with a hole is recoverable; an application whose MPI
// cannot send is not.
inline bool is_ibgda_uar(std::uint32_t flags) {
    return flags == kUarAllocNc || flags == kUarAllocNcDedicated;
}

// Swap this UAR's doorbell register for a page we watch.
//
// FAIL OPEN, like every other thing that sits in a data path here: if the decoy
// cannot be allocated the UAR is left exactly as libmlx5 handed it over and
// counted as unarmed. A capture with a hole is recoverable; an application that
// cannot ring its doorbell is not.
inline void arm(mlx5dv_devx_uar* uar) {
    if (!uar || !uar->reg_addr)
        return;
    if (!platform_refuses_gpu_doorbell(uar->reg_addr)) {
        state().uars_unarmed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& s = state();

    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (auto it = s.armed.find(uar); it != s.armed.end()) {
            ++it->second.refs;  // singleton NC UAR handed out again; already swapped
            return;
        }
    }

    void* real = uar->reg_addr;

    // A private, page-aligned, writable page. Plain host memory on purpose:
    // the writer in this mode is a CPU thread, so nothing here needs the CUDA
    // allocator the GPU hijack's fake page needs.
    void* decoy = ::mmap(nullptr, kUarPageSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (decoy == MAP_FAILED) {
        s.uars_unarmed.fetch_add(1, std::memory_order_relaxed);
        if (debug_on())
            std::fprintf(stderr, "[hostdb] mmap failed for uar=%p — FAIL OPEN\n", (void*)uar);
        return;
    }
    std::memset(decoy, 0, kUarPageSize);

    // The decode is the GPU path's, unchanged: same control segment, same send
    // queue, same segment layout. Only who wrote the doorbell differs, and the
    // decoder never needed to know that.
    repeater::RepeaterHandle handle(static_cast<std::uint64_t*>(decoy), real,
                                    [real](std::uint64_t val) { hijack::on_doorbell(real, val); });
    handle.start();

    {
        std::lock_guard<std::mutex> lk(s.mutex);
        Armed& a = s.armed[uar];
        a.decoy = decoy;
        a.real = real;
        a.refs = 1;
        a.handles.emplace_back(std::move(handle));
    }

    // THE SWAP. Everything above is set up; this line is what redirects the
    // application, and it is last so that no doorbell can land on a page whose
    // repeater is not yet running.
    uar->reg_addr = decoy;

    s.uars_armed.fetch_add(1, std::memory_order_relaxed);
    if (debug_on())
        std::fprintf(stderr, "[hostdb] SWAP uar=%p reg_addr %p -> %p (repeater armed)\n",
                     (void*)uar, real, decoy);
}

// Undo the swap. Restores reg_addr first so that anything still holding the UAR
// sees the real register, then stops the repeater and releases the page — in
// that order, because destroying a RepeaterHandle joins its thread and the page
// must outlive the join.
inline void disarm(mlx5dv_devx_uar* uar) {
    if (!uar)
        return;
    auto& s = state();

    std::vector<repeater::RepeaterHandle> to_join;
    void*                                 decoy = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        auto                        it = s.armed.find(uar);
        if (it == s.armed.end())
            return;
        if (--it->second.refs > 0)
            return;  // a singleton still in use by another queue pair
        uar->reg_addr = it->second.real;
        decoy = it->second.decoy;
        to_join = std::move(it->second.handles);
        s.armed.erase(it);
    }

    to_join.clear();  // joins every repeater thread before the page goes away
    if (decoy)
        ::munmap(decoy, kUarPageSize);
}

// Tear down whatever is still armed at exit, and say what was seen.
//
// The counters are read against the hijack's own, because the two paths share
// one decoder: doorbells and sends come from hijack::state(). What belongs here
// is only how many doorbell registers this run was actually watching.
inline void report_and_shutdown(int rank) {
    auto& s = state();

    std::vector<mlx5dv_devx_uar*> live;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        live.reserve(s.armed.size());
        for (auto& [uar, _] : s.armed)
            live.push_back(static_cast<mlx5dv_devx_uar*>(uar));
    }
    for (auto* uar : live) {
        {
            std::lock_guard<std::mutex> lk(s.mutex);
            if (auto it = s.armed.find(uar); it != s.armed.end())
                it->second.refs = 1;  // force the last release
        }
        disarm(uar);
    }

    auto& h = hijack::state();
    std::fprintf(stderr,
                 "[comshu rank %d] host doorbell: uars_armed=%u unarmed=%u doorbells=%llu "
                 "sends=%llu (unknown_op=%llu truncated=%llu) wqes_lapped=%llu%s\n",
                 rank, s.uars_armed.load(), s.uars_unarmed.load(),
                 static_cast<unsigned long long>(h.doorbells_decoded.load()),
                 static_cast<unsigned long long>(h.sends_emitted.load()),
                 static_cast<unsigned long long>(h.sends_unknown_op.load()),
                 static_cast<unsigned long long>(h.sends_truncated.load()),
                 static_cast<unsigned long long>(h.wqes_lapped.load()),
                 repeater::wait_is_spin() ? " (PAUSE spin — no WAITPKG/MWAITX)" : "");

    if (s.uars_armed.load() > 0 && h.doorbells_decoded.load() == 0)
        std::fprintf(stderr,
                     "[comshu rank %d] host doorbell: %u UAR(s) watched and NO doorbell rang "
                     "through them. Either this run sent nothing, or its doorbell writer is the "
                     "GPU — in which case this is the wrong mechanism and "
                     "--enable-doorbell-hijack is the right one.\n",
                     rank, s.uars_armed.load());
}

}  // namespace mlx5::hostdb
