#pragma once
// The UAR doorbell hijack — Stage 1: the swap, as a pure forwarder.
//
// Ported from SigmaTrace/Profiler/Handlers/CUDAHijack.cpp. Hooks the CUDA DRIVER
// API (cuMemHostRegister_v2 / cuMemHostGetDevicePointer_v2 / cuMemHostUnregister),
// reached through the export-table patcher in cuda_export_table.hpp — because on
// du04 NVSHMEM links libcudart statically and cudart_static calls libcuda's
// cuMemHostGetDevicePointer through the driver export table, not through any
// dynamic symbol or cuGetProcAddress. See cuda_export_table.hpp for the evidence.
//
// The mechanism, in the order the pieces exist:
//
//   0. mlx5dv_devx_alloc_uar hands out a doorbell page. The provider records its
//      reg_addr and calls remember_uar() here.
//   1. cudaHostRegister(reg_addr, …, Mapped) -> cudart_static -> cuMemHostRegister.
//      We recognise the UAR and SKIP the real registration — the real BAR page is
//      never mapped to the GPU; only the CPU (this hijack's repeater) writes it.
//   2. cudaHostGetDevicePointer(&ptr, reg_addr, …) -> cuMemHostGetDevicePointer.
//      THE SWAP: allocate a fake host page, park a repeater on it, and return the
//      fake page's device pointer. The GPU rings the fake page; the repeater
//      forwards each store to the real UAR, so the NIC still sends.
//
// Stage 1 attaches no decode: the repeater callback only counts, so a full
// NVSHMEM run can be measured for byte-identical output and overhead inside the
// noise of three clean runs. If that gate fails, no amount of correct parsing
// rescues a data path that perturbs the application, and the work stops here.
//
// THREE RULES, because this sits in the data path where every other pcomm hook
// is passive:
//   * Fail open, not closed. Cannot allocate the fake page, cannot resolve a
//     driver call, no UAR match -> return the REAL device pointer (registering
//     the page lazily, since step 1 skipped it) and count the UAR as unwatched.
//   * Never manufacture a capability. Only pages mlx5dv_devx_alloc_uar handed
//     out are swapped; everything else passes straight through.
//   * Forward unconditionally. That rule lives in memory_repeater.hpp, on the
//     store the repeater must never skip.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <endian.h>

#include "cuda_export_table.hpp"
#include "memory_repeater.hpp"

namespace mlx5::hijack {

// --- Stage 2 sinks ---------------------------------------------------------
// Records are emitted by the provider (where the Recorder and intern tables
// live), so the decode calls back out through these. Set once, from the
// provider constructor. Interning on the repeater thread is safe: the engine's
// intern tables are process-wide under a per-domain lock.
using DoorbellSink = void (*)(void* uar_ptr, std::uint32_t qp_num, std::uint32_t prod_idx);
using DeviceSendSink = void (*)(std::uint32_t qp_num, std::uint32_t wqe_idx, std::uint8_t opcode,
                                std::uint64_t laddr, std::uint32_t lkey, std::uint64_t raddr,
                                std::uint32_t rkey, std::uint32_t length);

// What the WQE copy-out needs to read a send queue: its GPU address and shape.
// Registered by the provider from the DEVX CREATE_QP decode, which is the only
// place qpn -> (sq_base, log_wq_size, service_type) is known.
struct QpInfo {
    std::uint64_t sq_gpu_base{0};   // device VA of the send queue
    std::uint32_t log_wq_size{0};   // WQEBBs = 1 << log_wq_size
    std::uint8_t  service_type{0};  // 0x5 = DCI (carries a datagram segment)
};

// CUDA driver types and flags, hand-copied from cuda.h and checked by nothing —
// the same exposure the rest of this provider carries, so it builds where CUDA
// does not. CUresult is an int; CUdeviceptr is 64-bit on LP64.
using CuResult = int;
using CuDevicePtr = unsigned long long;
inline constexpr CuResult kCudaSuccess = 0;
inline constexpr CuResult kCudaErrorInvalidValue = 1;

inline constexpr unsigned    kMemHostRegisterDeviceMap = 0x02;  // CU_MEMHOSTREGISTER_DEVICEMAP
inline constexpr unsigned    kMemHostAllocPortable = 0x01;      // CU_MEMHOSTALLOC_PORTABLE
inline constexpr unsigned    kMemHostAllocDeviceMap = 0x02;     // CU_MEMHOSTALLOC_DEVICEMAP
inline constexpr std::size_t kUarPageSize = 4096;

using PfnHostRegister = CuResult (*)(void*, std::size_t, unsigned);
using PfnHostGetDevicePointer = CuResult (*)(CuDevicePtr*, void*, unsigned);
using PfnHostUnregister = CuResult (*)(void*);
using PfnHostAlloc = CuResult (*)(void**, std::size_t, unsigned);
using PfnFreeHost = CuResult (*)(void*);

// The launcher's --enable-doorbell-hijack sets PCOMM_DOORBELL_HIJACK=1. Checked
// once; gates whether the provider installs the export-table patcher at all.
inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_DOORBELL_HIJACK");
        return v && *v == '1';
    }();
    return on;
}

inline bool debug_on() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_HIJACK_DEBUG");
        return v && *v == '1';
    }();
    return on;
}

// --- real libcuda driver functions -----------------------------------------
// Resolved through the export-table module: get_real returns the pointer it
// captured while patching, or resolves by name from the promoted-global libcuda.
inline PfnHostRegister real_host_register() {
    return reinterpret_cast<PfnHostRegister>(export_table::real("cuMemHostRegister_v2"));
}
inline PfnHostGetDevicePointer real_host_get_device_pointer() {
    return reinterpret_cast<PfnHostGetDevicePointer>(
        export_table::real("cuMemHostGetDevicePointer_v2"));
}
inline PfnHostAlloc real_host_alloc() {
    return reinterpret_cast<PfnHostAlloc>(export_table::real("cuMemHostAlloc"));
}
inline PfnFreeHost real_free_host() {
    return reinterpret_cast<PfnFreeHost>(export_table::real("cuMemFreeHost"));
}

struct RegisterIntent {
    std::size_t bytesize{0};
    unsigned    flags{0};
};

// Per-QP copy-out state: the running producer index, and a lazily-created CUDA
// stream + pinned host buffer the WQEs are copied into. One QP's doorbells all
// ring the same UAR, so one repeater thread owns this — but it is guarded anyway
// because teardown touches it from another thread.
struct QpCopy {
    std::mutex     mutex;
    std::uint32_t  last_prod{0};
    bool           have_last{false};
    void*          stream{nullptr};    // CUstream
    std::uint8_t*  host_buf{nullptr};  // pinned, 1<<log_wq_size WQEBBs
    std::size_t    host_buf_bytes{0};
};

struct State {
    std::mutex mutex;

    std::unordered_map<void*, std::uint32_t>          uar_pages;   // reg_addr -> page_id
    std::unordered_map<void*, void*>                  uar_ptrs;    // reg_addr -> mlx5dv_devx_uar*
    std::unordered_map<void*, RegisterIntent>         skipped;     // reg_addr -> skipped register
    std::unordered_map<void*, void*>                  fake_pages;  // reg_addr -> fake page
    std::unordered_map<void*, std::vector<repeater::RepeaterHandle>> handles;  // reg_addr -> repeaters

    // Stage 2.
    DoorbellSink                            doorbell_sink{nullptr};
    DeviceSendSink                          device_send_sink{nullptr};
    std::unordered_map<std::uint32_t, QpInfo> qp_info;   // qpn -> send-queue shape
    std::unordered_map<std::uint32_t, std::unique_ptr<QpCopy>> qp_copy;  // qpn -> copy state
    std::atomic<void*>                        gpu_ctx{nullptr};  // CUcontext captured at the swap

    std::atomic<std::uint64_t> num_fake_accesses{0};
    std::atomic<std::uint64_t> doorbells_decoded{0};
    std::atomic<std::uint64_t> sends_emitted{0};
    std::atomic<std::uint64_t> wqes_lapped{0};  // WQEBBs the GPU overwrote before we copied
    std::atomic<std::uint32_t> uars_hijacked{0};
    std::atomic<std::uint32_t> uars_unwatched{0};
    std::atomic<std::uint32_t> remembered_uars{0};
    std::atomic<std::uint32_t> register_calls{0};
    std::atomic<std::uint32_t> getdevptr_calls{0};
    std::atomic<std::uint32_t> unregister_calls{0};
};

inline State& state() {
    static State s;
    return s;
}

inline void remember_uar(void* reg_addr, void* uar_ptr, std::uint32_t page_id) {
    if (!reg_addr)
        return;
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        s.uar_pages[reg_addr] = page_id;
        s.uar_ptrs[reg_addr] = uar_ptr;
    }
    s.remembered_uars.fetch_add(1, std::memory_order_relaxed);
    if (debug_on())
        std::fprintf(stderr, "[hijack] remember_uar reg_addr=%p page_id=%u\n", reg_addr, page_id);
}

// The provider hands us each GPU-driven DEVX queue pair: its send-queue GPU
// address and shape. Without this the decode knows a doorbell rang and cannot
// read the WQE it announced.
inline void register_qp(std::uint32_t qp_num, std::uint64_t sq_gpu_base, std::uint32_t log_wq_size,
                        std::uint8_t service_type) {
    auto&                       s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.qp_info[qp_num] = QpInfo{sq_gpu_base, log_wq_size, service_type};
}

inline void set_sinks(DoorbellSink doorbell, DeviceSendSink device_send) {
    auto& s = state();
    s.doorbell_sink = doorbell;
    s.device_send_sink = device_send;
}

// --- CUDA driver calls for the WQE copy-out --------------------------------
// Resolved through the export-table module, like the fake-page allocation.
using CuDevicePtr2 = unsigned long long;
using PfnCtxGetCurrent = CuResult (*)(void**);
using PfnCtxSetCurrent = CuResult (*)(void*);
using PfnStreamCreate = CuResult (*)(void**, unsigned);
using PfnStreamSync = CuResult (*)(void*);
using PfnMemcpyDtoHAsync = CuResult (*)(void*, CuDevicePtr2, std::size_t, void*);

inline void* resolve2(std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (void* p = export_table::real(n))
            return p;
    return nullptr;
}

// --- doorbell decode + WQE copy-out ----------------------------------------

inline constexpr std::uint32_t kWqebb = 64;
inline constexpr std::uint32_t kProdMask = 0xFFFF;
inline std::uint32_t prod_diff(std::uint32_t end, std::uint32_t start) {
    return (end - start) & kProdMask;
}
inline bool opcode_has_raddr(std::uint8_t op) {
    // RDMA_WRITE, RDMA_READ, ATOMIC_CS/FA, ATOMIC_MASKED_CS/FA
    return op == 0x08 || op == 0x10 || op == 0x11 || op == 0x12 || op == 0x14 || op == 0x15;
}
inline bool opcode_has_data(std::uint8_t op) {
    return op == 0x08 || op == 0x0a || op == 0x10;  // RDMA_WRITE, SEND, RDMA_READ
}

// Copy the WQEBBs in [start, end) out of the send queue into `dst`, handling the
// ring wraparound. Ported from SigmaTrace copy_wqes_from_gpu.
inline CuResult copy_wqes(std::uint8_t* dst, const QpInfo& qp, std::uint32_t start,
                          std::uint32_t end, void* stream, PfnMemcpyDtoHAsync memcpy_fn) {
    const std::uint32_t total = prod_diff(end, start);
    const std::uint32_t slots = 1u << qp.log_wq_size;
    const std::uint32_t mask = slots - 1u;
    const std::uint32_t start_slot = start & mask;
    const auto          base = static_cast<CuDevicePtr2>(qp.sq_gpu_base);

    if (start_slot + total <= slots)
        return memcpy_fn(dst, base + static_cast<CuDevicePtr2>(start_slot) * kWqebb,
                         static_cast<std::size_t>(total) * kWqebb, stream);
    // Wraps the ring: tail then head.
    const std::uint32_t tail = slots - start_slot;
    const std::uint32_t head = total - tail;
    CuResult            rc = memcpy_fn(dst, base + static_cast<CuDevicePtr2>(start_slot) * kWqebb,
                                       static_cast<std::size_t>(tail) * kWqebb, stream);
    if (rc != kCudaSuccess)
        return rc;
    return memcpy_fn(dst + static_cast<std::size_t>(tail) * kWqebb, base,
                     static_cast<std::size_t>(head) * kWqebb, stream);
}

// Parse the WQEBBs copied into `buf` (covering producer indices [start, end))
// and emit one Mlx5DeviceSend per real WQE. Ported from SigmaTrace parse_and_emit,
// reduced to per-WQE emission (no doorbell-boundary batching — that is Stage 3's
// concern, and correctness comes first).
inline void parse_wqes(const std::uint8_t* buf, std::uint32_t start, std::uint32_t end,
                       std::uint32_t qpn, const QpInfo& qp, DeviceSendSink sink) {
    const std::uint32_t total = prod_diff(end, start);
    const bool          is_dci = qp.service_type == 0x5;
    std::uint32_t       i = 0, prod = start;

    while (i < total) {
        const std::uint8_t* wqe = buf + static_cast<std::size_t>(i) * kWqebb;
        const std::uint32_t qpn_ds = be32toh(*reinterpret_cast<const std::uint32_t*>(wqe + 4));
        const std::uint32_t ds = qpn_ds & 0xFF;
        if ((qpn_ds >> 8) != qpn || ds == 0) {  // NOP or another QP's slot
            ++i;
            prod = (prod + 1) & kProdMask;
            continue;
        }
        const std::uint8_t opcode =
            be32toh(*reinterpret_cast<const std::uint32_t*>(wqe)) & 0xFF;
        const std::uint32_t wqebbs = (ds + 3) >> 2;

        const bool has_raddr = opcode_has_raddr(opcode);
        const bool has_data = opcode_has_data(opcode);

        std::uint64_t raddr = 0, laddr = 0;
        std::uint32_t rkey = 0, lkey = 0, length = 0;

        // Segment positions, from the FRONT: ctrl(1 DS) · av · raddr(1 DS) ·
        // data|inline (ibgda_device.cuh). The address-vector segment is a HALF AV
        // (1 DS) on this system — confirmed by ds==4 for a simple pointer write
        // (ctrl+halfav+raddr+data) — so av_ds is 1 for DCI, 0 for RC. Locating by
        // ds from the END is wrong: NVSHMEM's warp-combine path INLINES the
        // payload (up to 128 B) into a segment that runs to the end of the WQE, so
        // the last DS is inline payload, not a descriptor. If support_half_av_seg
        // is ever false this av_ds becomes 3 (full AV, ds==6) — one place to fix.
        const std::uint32_t av_ds = is_dci ? 1u : 0u;
        const std::uint32_t raddr_ds = 1u + av_ds;
        const std::uint32_t op_ds = 2u + av_ds;  // data seg, inline header, or atomic seg
        auto seg_at = [&](std::uint32_t which_ds) {
            return wqe + static_cast<std::size_t>(which_ds) * 16;
        };
        if (has_raddr && ds > raddr_ds) {
            const std::uint8_t* r = seg_at(raddr_ds);
            raddr = be64toh(*reinterpret_cast<const std::uint64_t*>(r));
            rkey = be32toh(*reinterpret_cast<const std::uint32_t*>(r + 8));
        }
        if (has_data && ds > op_ds) {
            const std::uint8_t*  d = seg_at(op_ds);
            const std::uint32_t  bc = be32toh(*reinterpret_cast<const std::uint32_t*>(d));
            length = bc & 0x7FFFFFFF;
            // MLX5_INLINE_SEG (bit 31): the payload is inline and there is no
            // local descriptor — laddr/lkey stay 0. Otherwise it is a pointer
            // data segment carrying byte_count, lkey, addr.
            if ((bc & 0x80000000u) == 0) {
                lkey = be32toh(*reinterpret_cast<const std::uint32_t*>(d + 4));
                laddr = be64toh(*reinterpret_cast<const std::uint64_t*>(d + 8));
            }
        }

        // Emit only actual operations — those that move data or name a remote
        // address (RDMA/atomic). NVSHMEM interleaves NOP WQEs (opcode 0x00) for
        // fencing and alignment; those carry no address and are not sends, so
        // recording them would inflate the send count with empty rows.
        if ((has_data || has_raddr) && sink) {
            sink(qpn, prod, opcode, laddr, lkey, raddr, rkey, length);
            state().sends_emitted.fetch_add(1, std::memory_order_relaxed);
        }

        i += wqebbs;
        prod = (prod + wqebbs) & kProdMask;
    }
}

// The repeater callback: decode the doorbell, emit Mlx5Doorbell, then copy the
// delta WQEs out of the send queue, parse them, and emit Mlx5DeviceSend. Runs on
// the repeater thread BEFORE the store is forwarded to the NIC, so the SQ still
// holds the WQEs the GPU has not yet had recycled.
inline void on_doorbell(void* reg_addr, std::uint64_t val) {
    auto& s = state();
    s.num_fake_accesses.fetch_add(1, std::memory_order_relaxed);

    // mlx5 control segment, big-endian: high 32 bits = qpn_ds, low = opmod_idx.
    const std::uint32_t qpn_ds = be32toh(static_cast<std::uint32_t>(val >> 32));
    const std::uint32_t opmod = be32toh(static_cast<std::uint32_t>(val));
    const std::uint32_t qpn = qpn_ds >> 8;
    const std::uint32_t prod_idx = (opmod >> 8) & kProdMask;
    s.doorbells_decoded.fetch_add(1, std::memory_order_relaxed);

    // Mlx5Doorbell — emitted unconditionally, even when the WQE cannot be read.
    void* uar_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (auto it = s.uar_ptrs.find(reg_addr); it != s.uar_ptrs.end())
            uar_ptr = it->second;
    }
    if (s.doorbell_sink)
        s.doorbell_sink(uar_ptr, qpn, prod_idx);

    // Mlx5DeviceSend — best-effort: needs the QP's send-queue address, a CUDA
    // context on this thread, and the copy machinery. Any miss leaves the
    // doorbell row standing and simply emits no send row.
    QpInfo qp;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        auto                        it = s.qp_info.find(qpn);
        if (it == s.qp_info.end())
            return;  // doorbell recorded; send queue unknown
        qp = it->second;
    }
    if (qp.sq_gpu_base == 0 || !s.device_send_sink)
        return;

    // Resolve the copy machinery once.
    static PfnCtxSetCurrent   ctx_set = reinterpret_cast<PfnCtxSetCurrent>(
        resolve2({"cuCtxSetCurrent"}));
    static PfnStreamCreate    stream_create = reinterpret_cast<PfnStreamCreate>(
        resolve2({"cuStreamCreate"}));
    static PfnStreamSync      stream_sync = reinterpret_cast<PfnStreamSync>(
        resolve2({"cuStreamSynchronize"}));
    static PfnMemcpyDtoHAsync memcpy_fn = reinterpret_cast<PfnMemcpyDtoHAsync>(
        resolve2({"cuMemcpyDtoHAsync_v2", "cuMemcpyDtoHAsync"}));
    if (!ctx_set || !stream_create || !stream_sync || !memcpy_fn)
        return;

    // Bind this repeater thread to the application's CUDA context, once.
    static thread_local bool ctx_bound = false;
    if (!ctx_bound) {
        void* ctx = s.gpu_ctx.load(std::memory_order_acquire);
        if (!ctx)
            return;
        if (ctx_set(ctx) != kCudaSuccess)
            return;
        ctx_bound = true;
    }

    // Per-QP copy state.
    QpCopy* copy = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        auto&                       slot = s.qp_copy[qpn];
        if (!slot)
            slot = std::make_unique<QpCopy>();
        copy = slot.get();
    }

    std::lock_guard<std::mutex> lk(copy->mutex);
    if (!copy->have_last) {  // first doorbell: nothing to copy, just anchor
        copy->last_prod = prod_idx;
        copy->have_last = true;
        return;
    }
    std::uint32_t       start = copy->last_prod;
    const std::uint32_t end = prod_idx;
    std::uint32_t       delta = prod_diff(end, start);
    if (delta == 0)
        return;

    // If the GPU produced more than a full ring since we last looked, the
    // earliest WQEBBs have already been overwritten and are unrecoverable —
    // this is the CPU repeater lapping under a fast producer, the exact loss
    // the on-GPU repeater (Stage 3) exists to prevent. Copy the most recent
    // ring's worth and count the lost WQEBBs, rather than copy beyond the ring
    // and emit rows decoded from wrapped, wrong bytes. Silence here would be the
    // worst outcome: plausible send rows that are simply false.
    const std::uint32_t slots = 1u << qp.log_wq_size;
    if (delta > slots) {
        s.wqes_lapped.fetch_add(delta - slots, std::memory_order_relaxed);
        start = (end - slots) & kProdMask;
        delta = slots;
    }

    // Lazily create the stream and pinned host buffer (4 rings' worth, so a
    // large delta cannot overflow it).
    if (!copy->stream)
        if (stream_create(&copy->stream, 0x1 /* NON_BLOCKING */) != kCudaSuccess)
            return;
    const std::size_t need = static_cast<std::size_t>(1u << qp.log_wq_size) * kWqebb;
    if (copy->host_buf_bytes < need) {
        auto* host_alloc = real_host_alloc();
        if (!host_alloc)
            return;
        void* nb = nullptr;
        if (host_alloc(&nb, need, kMemHostAllocPortable) != kCudaSuccess || !nb)
            return;
        if (copy->host_buf)
            if (auto* free_host = real_free_host())
                free_host(copy->host_buf);
        copy->host_buf = static_cast<std::uint8_t*>(nb);
        copy->host_buf_bytes = need;
    }

    if (copy_wqes(copy->host_buf, qp, start, end, copy->stream, memcpy_fn) != kCudaSuccess)
        return;
    if (stream_sync(copy->stream) != kCudaSuccess)
        return;
    parse_wqes(copy->host_buf, start, end, qpn, qp, s.device_send_sink);
    copy->last_prod = end;
}

// Tear down the repeater(s) for a UAR and free its fake page. Idempotent and
// keyed on reg_addr, because free_uar and cuMemHostUnregister can arrive in
// either order and both call this. Destroying a RepeaterHandle stops delivery,
// stops the thread and joins it; the fake page is freed only after the join.
inline void teardown(void* reg_addr) {
    auto& s = state();

    std::vector<repeater::RepeaterHandle> to_join;
    void*                                 fake = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (auto it = s.handles.find(reg_addr); it != s.handles.end()) {
            to_join = std::move(it->second);
            s.handles.erase(it);
        }
        if (auto it = s.fake_pages.find(reg_addr); it != s.fake_pages.end()) {
            fake = it->second;
            s.fake_pages.erase(it);
        }
    }

    to_join.clear();  // joins every repeater thread before the page is freed

    if (fake)
        if (auto* free_host = real_free_host())
            free_host(fake);
}

// --- the three overwrites, installed into the driver export table ----------

inline CuResult intercept_host_register(void* p, std::size_t bytesize, unsigned flags) {
    auto* real = real_host_register();

    auto& s = state();
    s.register_calls.fetch_add(1, std::memory_order_relaxed);
    if ((flags & kMemHostRegisterDeviceMap) != 0) {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (s.uar_pages.count(p)) {
            s.skipped[p] = RegisterIntent{bytesize, flags};
            if (debug_on())
                std::fprintf(stderr, "[hijack] cuMemHostRegister UAR p=%p flags=0x%x -> SKIP real\n",
                             p, flags);
            return kCudaSuccess;
        }
    }
    if (debug_on())
        std::fprintf(stderr, "[hijack] cuMemHostRegister p=%p flags=0x%x -> passthrough\n", p, flags);
    return real ? real(p, bytesize, flags) : kCudaErrorInvalidValue;
}

inline CuResult intercept_host_get_device_pointer(CuDevicePtr* pdptr, void* p, unsigned flags) {
    auto* real = real_host_get_device_pointer();
    if (!real) {
        if (debug_on())
            std::fprintf(stderr, "[hijack] cuMemHostGetDevicePointer p=%p NO REAL FN\n", p);
        return kCudaErrorInvalidValue;
    }

    auto& s = state();
    s.getdevptr_calls.fetch_add(1, std::memory_order_relaxed);

    RegisterIntent intent;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        auto                        skip_it = s.skipped.find(p);
        if (skip_it == s.skipped.end()) {
            if (debug_on())
                std::fprintf(stderr,
                             "[hijack] cuMemHostGetDevicePointer p=%p -> passthrough (not UAR)\n", p);
            return real(pdptr, p, flags);  // not a UAR page we own — pass through
        }
        intent = skip_it->second;
        // Already swapped: hand back the same fake page's device pointer.
        if (auto fk = s.fake_pages.find(p); fk != s.fake_pages.end())
            return real(pdptr, fk->second, flags);
    }

    // Fail-open: give the application a REAL, working device pointer. Step 1
    // skipped the registration, so register it now, then resolve it directly.
    auto fail_open = [&]() -> CuResult {
        s.uars_unwatched.fetch_add(1, std::memory_order_relaxed);
        if (debug_on())
            std::fprintf(stderr, "[hijack] p=%p FAIL-OPEN -> real register+getdevptr\n", p);
        if (auto* reg = real_host_register())
            reg(p, intent.bytesize, intent.flags);
        return real(pdptr, p, flags);
    };

    // Allocate the fake page.
    auto* host_alloc = real_host_alloc();
    if (!host_alloc)
        return fail_open();
    void*          fake = nullptr;
    const CuResult rc =
        host_alloc(&fake, kUarPageSize, kMemHostAllocPortable | kMemHostAllocDeviceMap);
    if (rc != kCudaSuccess || !fake)
        return fail_open();
    std::memset(fake, 0, kUarPageSize);

    // Capture the application's CUDA context here, on the thread that owns it, so
    // the repeater thread can bind to it for the WQE copy-out.
    if (!s.gpu_ctx.load(std::memory_order_acquire)) {
        if (auto* getcur =
                reinterpret_cast<PfnCtxGetCurrent>(export_table::real("cuCtxGetCurrent"))) {
            void* ctx = nullptr;
            if (getcur(&ctx) == kCudaSuccess && ctx)
                s.gpu_ctx.store(ctx, std::memory_order_release);
        }
    }

    // Park a repeater on the fake page. The callback decodes the doorbell and the
    // WQEs it announced (Stage 2), then the repeater forwards the store.
    repeater::RepeaterHandle handle(static_cast<std::uint64_t*>(fake), p,
                                    [p](std::uint64_t val) { on_doorbell(p, val); });
    handle.start();

    {
        std::lock_guard<std::mutex> lk(s.mutex);
        s.fake_pages[p] = fake;
        s.handles[p].emplace_back(std::move(handle));
    }
    s.uars_hijacked.fetch_add(1, std::memory_order_relaxed);
    if (debug_on())
        std::fprintf(stderr, "[hijack] SWAP p=%p -> fake=%p (repeater armed)\n", p, fake);

    return real(pdptr, fake, flags);
}

inline CuResult intercept_host_unregister(void* p) {
    auto* real = reinterpret_cast<PfnHostUnregister>(export_table::real("cuMemHostUnregister"));

    auto& s = state();
    s.unregister_calls.fetch_add(1, std::memory_order_relaxed);
    bool was_uar = false;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        was_uar = s.skipped.erase(p) > 0;
    }
    if (was_uar) {
        teardown(p);  // we skipped the register, so skip the unregister too
        return kCudaSuccess;
    }
    return real ? real(p) : kCudaErrorInvalidValue;
}

// Register the three overwrites with the export-table patcher and arm it. Call
// once from the provider constructor when enabled.
inline void install() {
    export_table::register_overwrite("cuMemHostRegister_v2",
                                     reinterpret_cast<void*>(&intercept_host_register));
    export_table::register_overwrite("cuMemHostGetDevicePointer_v2",
                                     reinterpret_cast<void*>(&intercept_host_get_device_pointer));
    export_table::register_overwrite("cuMemHostUnregister",
                                     reinterpret_cast<void*>(&intercept_host_unregister));
    export_table::install();
}

// Free repeater threads and pages still standing at shutdown, and report. The
// counts are the Stage-1 gate's evidence: num_fake_accesses > 0 means the GPU
// rang a page we owned and the man-in-the-middle engaged.
inline void report_and_shutdown(int rank) {
    auto& s = state();

    std::vector<void*> live;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        live.reserve(s.handles.size());
        for (auto& [addr, _] : s.handles)
            live.push_back(addr);
    }
    for (void* addr : live)
        teardown(addr);

    std::fprintf(stderr,
                 "[pcomm rank %d] doorbell hijack: uars_hijacked=%u unwatched=%u "
                 "fake_accesses=%llu doorbells=%llu sends=%llu wqes_lapped=%llu | "
                 "remembered_uars=%u register_calls=%u getdevptr_calls=%u unregister_calls=%u%s\n",
                 rank, s.uars_hijacked.load(), s.uars_unwatched.load(),
                 static_cast<unsigned long long>(s.num_fake_accesses.load()),
                 static_cast<unsigned long long>(s.doorbells_decoded.load()),
                 static_cast<unsigned long long>(s.sends_emitted.load()),
                 static_cast<unsigned long long>(s.wqes_lapped.load()), s.remembered_uars.load(),
                 s.register_calls.load(), s.getdevptr_calls.load(), s.unregister_calls.load(),
                 repeater::wait_is_spin() ? " (PAUSE spin — no WAITPKG/MWAITX)" : "");
}

}  // namespace mlx5::hijack
