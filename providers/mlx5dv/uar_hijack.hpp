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
// THREE RULES, because this sits in the data path where every other comshu hook
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
// `submitter` says which door saw the WQE (kSubmitter* below). `ticks_lo` and
// `ticks_hi` say when: 0/0 from the doorbell paths, which call the sink at the
// doorbell itself, so the row's time is now; a window from the send-queue
// watcher, which knows only that the WQE was published between two of its looks.
using DeviceSendSink = void (*)(std::uint32_t qp_num, std::uint32_t wqe_idx, std::uint8_t opcode,
                                std::uint64_t laddr, std::uint32_t lkey, std::uint64_t raddr,
                                std::uint32_t rkey, std::uint32_t length, std::uint32_t dest_dct,
                                std::uint16_t dest_lid, std::uint64_t dest_gid_prefix,
                                std::uint64_t dest_gid_iface, std::uint8_t decode, std::uint8_t av_ds,
                                std::uint8_t submitter,
                                std::uint64_t ticks_lo, std::uint64_t ticks_hi);

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

inline constexpr unsigned    kMemHostRegisterPortable = 0x01;   // CU_MEMHOSTREGISTER_PORTABLE
inline constexpr unsigned    kMemHostRegisterDeviceMap = 0x02;  // CU_MEMHOSTREGISTER_DEVICEMAP
inline constexpr unsigned    kMemHostRegisterIoMemory = 0x04;   // CU_MEMHOSTREGISTER_IOMEMORY
inline constexpr CuResult    kCudaErrorNotPermitted = 800;      // CUDA_ERROR_NOT_PERMITTED
// What NVSHMEM registers of a doorbell page: 8 bytes at the (unaligned)
// reg_addr, not a page — IBGDA_MLX5_NC_UAR_SIZE in its transport.
inline constexpr std::size_t kDoorbellRegBytes = 8;
inline constexpr unsigned    kMemHostAllocPortable = 0x01;      // CU_MEMHOSTALLOC_PORTABLE
inline constexpr unsigned    kMemHostAllocDeviceMap = 0x02;     // CU_MEMHOSTALLOC_DEVICEMAP
inline constexpr std::size_t kUarPageSize = 4096;

using PfnHostRegister = CuResult (*)(void*, std::size_t, unsigned);
using PfnHostGetDevicePointer = CuResult (*)(CuDevicePtr*, void*, unsigned);
using PfnHostUnregister = CuResult (*)(void*);
using PfnHostAlloc = CuResult (*)(void**, std::size_t, unsigned);
using PfnFreeHost = CuResult (*)(void*);

// The launcher's --enable-doorbell-hijack sets COMSHU_DOORBELL_HIJACK=1. Checked
// once; gates whether the provider installs the export-table patcher at all.
inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_DOORBELL_HIJACK");
        return v && *v == '1';
    }();
    return on;
}

// MEASUREMENT ONLY: forward the doorbell and emit Mlx5Doorbell, but do not copy
// the WQEs out of the send queue.
//
// This exists to answer one question that decides whether the on-GPU repeater
// (PLAN-20260807-2344) is worth building: the hijack's cost is either the HOST
// ROUND TRIP on the forward, which only moving the repeater onto the GPU can
// remove, or the per-doorbell `cuMemcpyDtoHAsync` + `cuStreamSynchronize` in the
// decode, which is a far cheaper thing to fix. The published +68% (forward) and
// +86% (forward + decode) were wall-clock over a run whose startup dominated, so
// they cannot separate the two; running the same workload with and without this
// switch can.
//
// NOT a supported capture mode, and it must never become one: it produces
// Mlx5Doorbell rows with no Mlx5DeviceSend beside them, which is exactly the
// "the doorbell rang and we could not decode it" shape that a real decode
// failure has. A capture taken this way is indistinguishable from a broken one,
// which is why it is spelled COMSHU_HIJACK_NO_DECODE and gated behind the
// hijack's own env rather than given a launcher flag.
inline bool decode_disabled() {
    static const bool off = [] {
        const char* v = std::getenv("COMSHU_HIJACK_NO_DECODE");
        return v && *v == '1';
    }();
    return off;
}

inline bool debug_on() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_HIJACK_DEBUG");
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
    std::mutex    mutex;
    std::uint32_t last_prod{0};
    bool          have_last{false};
    void*         stream{nullptr};    // CUstream
    std::uint8_t* host_buf{nullptr};  // pinned, 1<<log_wq_size WQEBBs
    std::size_t   host_buf_bytes{0};
};

struct State {
    std::mutex mutex;

    std::unordered_map<void*, std::uint32_t>  uar_pages;   // reg_addr -> page_id
    std::unordered_map<void*, void*>          uar_ptrs;    // reg_addr -> mlx5dv_devx_uar*
    std::unordered_map<void*, RegisterIntent> skipped;     // reg_addr -> skipped register
    std::unordered_map<void*, void*>          fake_pages;  // reg_addr -> fake page
    std::unordered_map<void*, std::vector<repeater::RepeaterHandle>>
        handles;  // reg_addr -> repeaters

    // Stage 2.
    DoorbellSink                                               doorbell_sink{nullptr};
    DeviceSendSink                                             device_send_sink{nullptr};
    std::unordered_map<std::uint32_t, QpInfo>                  qp_info;  // qpn -> send-queue shape
    std::unordered_map<std::uint32_t, std::unique_ptr<QpCopy>> qp_copy;  // qpn -> copy state
    std::atomic<void*> gpu_ctx{nullptr};  // CUcontext captured at the swap

    std::atomic<std::uint64_t> num_fake_accesses{0};
    std::atomic<std::uint64_t> doorbells_decoded{0};
    std::atomic<std::uint64_t> sends_emitted{0};
    std::atomic<std::uint64_t> sends_unknown_op{0};  // rows whose opcode this build cannot lay out
    std::atomic<std::uint64_t> sends_truncated{0};   // rows whose ds did not cover their segments
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

// Capture the application's CUDA context from a thread that has one, so the
// repeater thread can bind to it and copy WQEs out of the send queue.
//
// The GPU hijack gets this for free at the swap: cuMemHostGetDevicePointer runs
// on the application's own thread. The HOST doorbell path (host_doorbell.hpp)
// never sees that call — there is no device mapping of the UAR in that mode —
// so it calls this from the DEVX CREATE_QP decode instead, which the transport
// issues from a thread that owns a context. Without it the copy-out has no
// context to bind to, and the capture is doorbells with no sends beside them:
// the shape a real decode failure has, which this provider refuses to produce
// by accident.
//
// Idempotent, and it promotes libcuda to global scope first because in host mode
// nothing has patched the driver export table, so `real()` must fall back to
// dlsym and dlsym must be able to see the driver.
inline void capture_gpu_ctx() {
    auto& s = state();
    if (s.gpu_ctx.load(std::memory_order_acquire))
        return;
    export_table::promote_libcuda();
    if (auto* getcur = reinterpret_cast<PfnCtxGetCurrent>(export_table::real("cuCtxGetCurrent"))) {
        void* ctx = nullptr;
        if (getcur(&ctx) == kCudaSuccess && ctx)
            s.gpu_ctx.store(ctx, std::memory_order_release);
    }
}

// --- doorbell decode + WQE copy-out ----------------------------------------

inline constexpr std::uint32_t kWqebb = 64;
inline constexpr std::uint32_t kProdMask = 0xFFFF;
inline constexpr std::uint32_t kExtendedAv = 0x80000000u;  // MLX5_EXTENDED_UD_AV
inline constexpr std::uint32_t kInlineSeg = 0x80000000u;   // MLX5_INLINE_SEG
inline constexpr std::uint8_t  kServiceTypeDci = 0x5;
inline std::uint32_t           prod_diff(std::uint32_t end, std::uint32_t start) {
    return (end - start) & kProdMask;
}

// mlx5 send opcodes whose segment layout this decoder knows. Values from
// rdma-core `providers/mlx5/mlx5dv.h`; DUMP is NVSHMEM's own and appears in no
// rdma-core header (`IBGDA_MLX5_OPCODE_DUMP` in ibgda_device.cuh).
enum : std::uint8_t {
    kOpNop = 0x00,
    kOpRdmaWrite = 0x08,
    kOpRdmaWriteImm = 0x09,
    kOpSend = 0x0a,
    kOpSendImm = 0x0b,
    kOpRdmaRead = 0x10,
    kOpAtomicCs = 0x11,
    kOpAtomicFa = 0x12,
    kOpAtomicMaskedCs = 0x14,
    kOpAtomicMaskedFa = 0x15,
    kOpDump = 0x23,
};

// How much of a WQE the decode actually read, carried on the row.
//
// "We read every field" and "we met a WQE whose layout this build does not
// know" must not look the same in the capture. That is the same rule
// Mlx5DevxCmd.decoded already applies to undecoded DEVX commands, and it is the
// rule the old parser broke: an unrecognised opcode was skipped in silence,
// justified by a comment about NOP padding that is true only of opcode 0x00.
// NVSHMEM emits DUMP (0x23) on its ordering path and both masked-atomic
// opcodes; none of the three were in the old tables, so all three vanished.
enum : std::uint8_t {
    kDecodeFull = 0,       // every segment the opcode names was located and read
    kDecodePartial = 1,    // remote address read, local descriptor not (atomics)
    kDecodeTruncated = 2,  // ds is too small for the opcode's own segments
    kDecodeUnknownOp = 3,  // opcode not in the table above; only ids are meaningful
};

// Which segments an opcode carries, in the order mlx5 packs them:
//   ctrl · [address vector] · [remote address] · [atomic operands] · [data|inline]
struct OpShape {
    bool known{false};
    bool addressed{false};  // takes an address vector when the QP is a DCI
    bool raddr{false};      // carries a remote-address segment
    bool data{false};       // carries a local data or inline segment we can read
    // An atomic puts operand segments between the remote address and the local
    // result descriptor, so `data` cannot be found by counting forward from
    // raddr the way every other opcode's is. It is found by counting BACK from
    // the end instead — see kDataSegIsLast at the read site.
    bool atomic{false};
};

inline OpShape op_shape(std::uint8_t op) {
    switch (op) {
        case kOpRdmaWrite:
        case kOpRdmaWriteImm:
        case kOpRdmaRead:
            return {true, true, true, true};
        case kOpSend:
        case kOpSendImm:
            return {true, true, false, true};
        case kOpAtomicCs:
        case kOpAtomicFa:
        case kOpAtomicMaskedCs:
        case kOpAtomicMaskedFa:
            // Operand segments sit between the remote address and the local
            // result descriptor, and their count is not fixed. This used to give
            // up there and emit Partial with length = 0, which was honest but
            // meant every atomic contributed nothing to any byte total — and
            // the wire check, which sums decoded lengths, silently counted
            // atomic traffic as zero.
            //
            // It does not need the operand widths. The local data segment is
            // the LAST segment of the work-queue entry, so it is at ds - 1 no
            // matter how many operand segments precede it. NVSHMEM's
            // ibgda_write_amo_wqe builds exactly that: ctrl + [AV] + raddr +
            // atomic_seg_1 [+ atomic_seg_2] + data, with the second operand
            // segment present only for an 8-byte MASKED_CS (`++ds`). Reading it
            // from the end costs nothing and covers both.
            return {true, true, true, true, /*atomic=*/true};
        case kOpDump:
            // NVSHMEM's ordering WQE (`ibgda_write_dump_wqe`): ctrl + data,
            // ds = 2, and NO address vector even on a DCI. It moves no payload
            // to a peer, but it is how the ordering the transport pays for
            // reaches the wire, so it is recorded and left distinguishable by
            // opcode rather than dropped.
            return {true, false, false, true};
        default:
            return {};
    }
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

// Mlx5DeviceSend.submitter: which door saw the WQE a row was decoded from, and
// therefore what its time means. Mirrors records::SendSubmitter — this header
// cannot include the records — and the provider static_asserts the two equal.
// 0 is deliberately "unknown", so a call site that forgets to say is visible.
inline constexpr std::uint8_t kSubmitterGpuDoorbell = 1;   // this file: the GPU's doorbell store
inline constexpr std::uint8_t kSubmitterHostDoorbell = 2;  // host_doorbell.hpp: a host thread's
inline constexpr std::uint8_t kSubmitterSqWatch = 3;       // sq_watch.hpp: the doorbell record

// Parse the WQEBBs copied into `buf` (covering producer indices [start, end))
// and emit one Mlx5DeviceSend per real WQE. Ported from SigmaTrace parse_and_emit,
// reduced to per-WQE emission (no doorbell-boundary batching — that is Stage 3's
// concern, and correctness comes first).
// What one parse emitted, so a door can keep its own counts. `state()`'s
// counters below are process-wide and shared by every door that decodes here;
// a door that reports its coverage needs the rows that were its own.
struct ParseCounts {
    std::uint32_t rows{0};
    std::uint32_t unknown_op{0};
    std::uint32_t truncated{0};
};

inline ParseCounts parse_wqes(const std::uint8_t* buf, std::uint32_t start, std::uint32_t end,
                              std::uint32_t qpn, const QpInfo& qp, DeviceSendSink sink,
                              std::uint8_t submitter, std::uint64_t ticks_lo,
                              std::uint64_t ticks_hi) {
    ParseCounts         counts;
    const std::uint32_t total = prod_diff(end, start);
    const bool          is_dci = qp.service_type == kServiceTypeDci;
    std::uint32_t       i = 0, prod = start;

    while (i < total) {
        const std::uint8_t* wqe = buf + static_cast<std::size_t>(i) * kWqebb;
        const std::uint32_t qpn_ds = be32toh(*reinterpret_cast<const std::uint32_t*>(wqe + 4));
        const std::uint32_t ds = qpn_ds & 0xFF;
        if ((qpn_ds >> 8) != qpn || ds == 0) {  // another QP's slot, or an empty one
            ++i;
            prod = (prod + 1) & kProdMask;
            continue;
        }
        const std::uint8_t opcode = be32toh(*reinterpret_cast<const std::uint32_t*>(wqe)) & 0xFF;

        // Advance by what the WQE claims, but never read past what was copied:
        // the buffer holds one ring, and a WQE at the end of the delta can claim
        // WQEBBs the copy did not cover.
        std::uint32_t wqebbs = (ds + 3) >> 2;
        if (i + wqebbs > total)
            wqebbs = total - i;
        const std::uint32_t readable_ds = wqebbs * 4;  // DS the copy actually holds

        if (opcode == kOpNop) {  // fencing and alignment filler, not an operation
            i += wqebbs;
            prod = (prod + wqebbs) & kProdMask;
            continue;
        }

        const OpShape shape = op_shape(opcode);
        std::uint8_t  decode = shape.known ? kDecodeFull : kDecodeUnknownOp;

        std::uint64_t raddr = 0, laddr = 0;
        std::uint32_t rkey = 0, lkey = 0, length = 0, dest_dct = 0;
        std::uint16_t dest_lid = 0;
        std::uint64_t dest_gid_prefix = 0, dest_gid_iface = 0;

        auto seg_at = [&](std::uint32_t which_ds) {
            return wqe + static_cast<std::size_t>(which_ds) * 16;
        };
        auto have = [&](std::uint32_t which_ds, std::uint32_t n_ds) {
            return which_ds + n_ds <= ds && which_ds + n_ds <= readable_ds;
        };

        // --- the address vector, and its width, which is not a constant ------
        //
        // A DC WQE carries an address vector; an RC WQE does not. Its WIDTH is
        // the part that must be read rather than assumed. NVSHMEM uses a 16-byte
        // compact AV (1 DS, ds=4 for a pointer write) when the port allows it and
        // a 48-byte full AV (3 DS, ds=6) when it does not — and it does not on
        // every RoCE port (lid == 0), every GRH-only IB port, when IB_FORCE_GRH
        // is set, when the LID-only AH is rejected, and on any HCA without
        // compact-AV support. All five branches are `support_half_av_seg = false`
        // in ibgda.cpp; the old parser assumed the compact form unconditionally,
        // which on any of them read raddr, rkey, lkey, laddr and length 32 bytes
        // early and emitted a row that was wrong rather than absent.
        //
        // The WQE says which form it is, so nothing needs to be plumbed in from
        // the transport. `dqp_dct` sits at AV offset 8 in BOTH forms —
        // ibgda_half_av_seg_t and struct mlx5_wqe_av share their first 16 bytes
        // (be64 key, be32 dqp_dct, stat_rate_sl, fl_mlid, be16 rlid) — and its
        // top bit is MLX5_EXTENDED_UD_AV, set only for the full form. NVSHMEM
        // says so on the compact struct itself: "The ext flag (in dqp_dct) must
        // be set to disable."
        //
        // The low 24 bits are the destination DCT. Reading them is what makes a
        // DC capture answer who a send went to; without it the whole transport
        // records traffic with no peer.
        std::uint32_t av_ds = 0;
        if (is_dci && shape.addressed) {
            if (!have(1, 1)) {
                decode = kDecodeTruncated;
            } else {
                const std::uint32_t dqp_dct =
                    be32toh(*reinterpret_cast<const std::uint32_t*>(seg_at(1) + 8));
                av_ds = (dqp_dct & kExtendedAv) ? 3u : 1u;
                dest_dct = dqp_dct & 0x00FFFFFFu;
                // rlid, the be16 at AV offset 14 in both forms. With dest_dct
                // it is the (LID, DCT) pair that names the target rank on
                // InfiniBand, since DCT numbers repeat across HCAs. On RoCE v2
                // mlx5 puts the UDP source port here instead, so it is kept raw
                // and the GID below is what names the target.
                dest_lid = be16toh(*reinterpret_cast<const std::uint16_t*>(seg_at(1) + 14));
                if (!have(1, av_ds)) {
                    decode = kDecodeTruncated;
                } else if (av_ds == 3) {
                    // The full form's second half (struct mlx5_wqe_av):
                    // be32 grh_gid_fl at 28, then rgid[16] at 32 — AV units 2
                    // and 3, which have(1, av_ds) has just proven readable.
                    //
                    // rgid is read unconditionally — a zero rgid simply decodes
                    // as "no GID" — and NOT gated on a GRH flag. Read off the wire (COMSHU_HIJACK_DEBUG,
                    // MN5 job 48929358, NVSHMEM over RoCE v2): grh_gid_fl was
                    // 00 30 00 00 — sgid index 3 << 20 and NO flag bit at all; on
                    // Ethernet the GRH is implied and rdma-core does not mark it.
                    // A bit-30 gate (the kernel's spelling) decoded dest_gid = 0
                    // on all 41,027 RoCE sends of case 25, and so did adding bit
                    // 31. An AV without a GRH has a zeroed rgid, so nonzero is
                    // the test that holds on both link layers.
                    //
                    // That makes this the one place the AV's raw bytes are
                    // trusted, so they stay inspectable:
                    // COMSHU_HIJACK_DEBUG=1 prints the first four full AVs.
                    if (debug_on()) {
                        static std::atomic<int> dumped{0};
                        if (dumped.fetch_add(1, std::memory_order_relaxed) < 4) {
                            const auto* b = reinterpret_cast<const unsigned char*>(seg_at(1));
                            char        hex[48 * 3 + 1];
                            for (int k = 0; k < 48; ++k)
                                std::snprintf(hex + 3 * k, 4, "%02x ", b[k]);
                            std::fprintf(stderr, "[comshu] full AV qpn=%u: %s\n", qpn, hex);
                        }
                    }
                    {
                        std::uint64_t v = 0;
                        std::memcpy(&v, seg_at(3), 8);
                        dest_gid_prefix = be64toh(v);
                        std::memcpy(&v, seg_at(3) + 8, 8);
                        dest_gid_iface = be64toh(v);
                    }
                }
            }
        }

        const std::uint32_t raddr_ds = 1u + av_ds;
        if (decode != kDecodeTruncated && shape.raddr) {
            if (!have(raddr_ds, 1)) {
                decode = kDecodeTruncated;
            } else {
                const std::uint8_t* r = seg_at(raddr_ds);
                raddr = be64toh(*reinterpret_cast<const std::uint64_t*>(r));
                rkey = be32toh(*reinterpret_cast<const std::uint32_t*>(r + 8));
            }
        }

        // The local data segment: one past the remote address for an ordinary
        // opcode, and the LAST segment for an atomic, whose operand segments sit
        // in between at a width this decoder deliberately does not model.
        std::uint32_t data_ds = raddr_ds + (shape.raddr ? 1u : 0u);
        if (shape.atomic) {
            // Guard the shape rather than trusting ds. `data_ds` currently points
            // at the FIRST operand segment, so everything from there to the end
            // is the operands plus the one data segment: 2 for a single-operand
            // atomic, 3 for the 8-byte masked compare-swap that carries two.
            //
            // Anything else is a work-queue entry built by something whose layout
            // this has not seen, and taking ds - 1 as its data segment would then
            // be a confident guess at the wrong 16 bytes — the same mistake the
            // address vector taught, where assuming one width read five fields
            // 32 bytes early and emitted a row that was wrong rather than absent.
            // Stay Partial and say so.
            const std::uint32_t tail_ds = (ds > data_ds) ? ds - data_ds : 0u;
            if (tail_ds < 2u || tail_ds > 3u)
                decode = kDecodePartial;
            else
                data_ds = ds - 1u;
        }
        if (decode != kDecodeTruncated && decode != kDecodePartial && shape.data) {
            if (!have(data_ds, 1)) {
                decode = kDecodeTruncated;
            } else {
                const std::uint8_t* d = seg_at(data_ds);
                const std::uint32_t bc = be32toh(*reinterpret_cast<const std::uint32_t*>(d));
                length = bc & ~kInlineSeg;
                // MLX5_INLINE_SEG (bit 31): the payload is inline and there is no
                // local descriptor — laddr/lkey stay 0. Otherwise it is a pointer
                // data segment carrying byte_count, lkey, addr.
                if ((bc & kInlineSeg) == 0) {
                    lkey = be32toh(*reinterpret_cast<const std::uint32_t*>(d + 4));
                    laddr = be64toh(*reinterpret_cast<const std::uint64_t*>(d + 8));
                }
            }
        }
        // There used to be an `else if` here turning any known opcode with no
        // readable data segment into Partial. It existed solely for the atomics,
        // which now have one, and every remaining known opcode carries data — so
        // it was unreachable. Removed rather than left as a comment-shaped guard:
        // a branch that cannot run is not a safety net, it is a claim that
        // something is handled.

        // Every WQE that is not padding gets a row, including one carrying an
        // opcode this build does not know. An unrecognised opcode is the version
        // boundary this provider is most likely to hit, and a capture that
        // quietly omits it is indistinguishable from a run that did not send.
        if (sink) {
            sink(qpn, prod, opcode, laddr, lkey, raddr, rkey, length, dest_dct, dest_lid,
                 dest_gid_prefix, dest_gid_iface, decode,
                 static_cast<std::uint8_t>(av_ds), submitter, ticks_lo, ticks_hi);
            state().sends_emitted.fetch_add(1, std::memory_order_relaxed);
            ++counts.rows;
            if (decode == kDecodeUnknownOp) {
                state().sends_unknown_op.fetch_add(1, std::memory_order_relaxed);
                ++counts.unknown_op;
            } else if (decode == kDecodeTruncated) {
                state().sends_truncated.fetch_add(1, std::memory_order_relaxed);
                ++counts.truncated;
            }
        }

        i += wqebbs;
        prod = (prod + wqebbs) & kProdMask;
    }
    return counts;
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

    // Measurement switch, checked here so everything above it — the forward and
    // the doorbell row — is identical between the two configurations, and the
    // only difference is the copy-out below.
    if (decode_disabled())
        return;

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
    static PfnCtxSetCurrent ctx_set =
        reinterpret_cast<PfnCtxSetCurrent>(resolve2({"cuCtxSetCurrent"}));
    static PfnStreamCreate stream_create =
        reinterpret_cast<PfnStreamCreate>(resolve2({"cuStreamCreate"}));
    static PfnStreamSync stream_sync =
        reinterpret_cast<PfnStreamSync>(resolve2({"cuStreamSynchronize"}));
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
    if (!copy->have_last) {
        // A queue pair this provider saw CREATED starts with its producer index
        // at zero, so the first doorbell announcing prod_idx = N is announcing
        // WQEs [0, N). There is something to copy, and anchoring at prod_idx
        // threw it away — one send per queue pair, silently, on every run since
        // the decode was written. MN5 job 45497029 shows it exactly: every
        // active QP reports doorbells = sends + 1, and no counter said so.
        //
        // Zero is the right anchor precisely because register_qp is called from
        // the CREATE_QP decode, before the queue pair can have sent anything.
        // If that ever stops holding, the delta > slots clamp below bounds the
        // read to one ring and wqes_lapped records the loss, rather than letting
        // a wrong anchor walk over stale bytes and emit rows decoded from them.
        copy->last_prod = 0;
        copy->have_last = true;
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
    // Both doorbell paths decode here, and they never share a run —
    // host_doorbell.hpp refuses to arm when the GPU hijack is on — so whichever
    // is live is who saw this WQE. Time 0/0: the sink runs at the doorbell.
    parse_wqes(copy->host_buf, start, end, qpn, qp, s.device_send_sink,
               enabled() ? kSubmitterGpuDoorbell : kSubmitterHostDoorbell, 0, 0);
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
                std::fprintf(stderr,
                             "[hijack] cuMemHostRegister UAR p=%p flags=0x%x -> SKIP real\n", p,
                             flags);
            return kCudaSuccess;
        }
    }
    if (debug_on())
        std::fprintf(stderr, "[hijack] cuMemHostRegister p=%p flags=0x%x -> passthrough\n", p,
                     flags);
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
                             "[hijack] cuMemHostGetDevicePointer p=%p -> passthrough (not UAR)\n",
                             p);
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
                 "[comshu rank %d] doorbell hijack: uars_hijacked=%u unwatched=%u "
                 "fake_accesses=%llu doorbells=%llu sends=%llu (unknown_op=%llu truncated=%llu) "
                 "wqes_lapped=%llu | "
                 "remembered_uars=%u register_calls=%u getdevptr_calls=%u unregister_calls=%u%s\n",
                 rank, s.uars_hijacked.load(), s.uars_unwatched.load(),
                 static_cast<unsigned long long>(s.num_fake_accesses.load()),
                 static_cast<unsigned long long>(s.doorbells_decoded.load()),
                 static_cast<unsigned long long>(s.sends_emitted.load()),
                 static_cast<unsigned long long>(s.sends_unknown_op.load()),
                 static_cast<unsigned long long>(s.sends_truncated.load()),
                 static_cast<unsigned long long>(s.wqes_lapped.load()), s.remembered_uars.load(),
                 s.register_calls.load(), s.getdevptr_calls.load(), s.unregister_calls.load(),
                 repeater::wait_is_spin() ? " (PAUSE spin — no WAITPKG/MWAITX)" : "");

    // ...and then say what those counters MEAN, because the most likely outcome
    // on a production cluster is an empty data plane that looks like a bug.
    //
    // The hijack hangs off one event: the transport mapping a NIC BAR page into
    // the GPU's address space (cuMemHostRegister of the UAR, then
    // cuMemHostGetDevicePointer). Many production clusters do not permit that —
    // the open kernel module has no PeerMappingOverride, and cudaHostRegister on
    // the UAR returns cudaErrorNotPermitted. MN5 is one of them. NVSHMEM then
    // falls back to CPU-assisted IBGDA (NVSHMEM_IBGDA_NIC_HANDLER=cpu /
    // cpu_host_memory, `use_async_postsend` on the device) AT EXIT CODE 0: the
    // GPU still builds every WQE in GPU memory and advances a producer index,
    // but a HOST thread rings the doorbell. There is no GPU store to a UAR to
    // intercept, so mlx5_device_send is empty — and empty for a platform reason,
    // not because the run sent nothing and not because the hijack broke.
    //
    // Distinguishing those three is the whole point of this provider, so it is
    // stated here rather than left to whoever reads the empty table.
    const bool     uars_exist = s.remembered_uars.load() > 0;
    const unsigned watched = s.uars_hijacked.load();
    const unsigned unwatched = s.uars_unwatched.load();
    if (uars_exist && watched == 0 && unwatched == 0) {
        std::fprintf(stderr,
                     "[comshu rank %d] doorbell hijack: DEVX allocated %u UAR(s) and NONE was "
                     "mapped into the GPU — the doorbell writer on this run is NOT the GPU. "
                     "This is CPU-assisted IBGDA or a host-submitted path, not a capture "
                     "failure; mlx5_device_send will be empty and no hijack can fill it. "
                     "Check the transport's own log for its NIC handler.\n",
                     rank, s.remembered_uars.load());
    } else if (watched > 0 && s.num_fake_accesses.load() == 0) {
        std::fprintf(stderr,
                     "[comshu rank %d] doorbell hijack: %u UAR(s) swapped and never rung — the "
                     "GPU issued no doorbell through them.\n",
                     rank, watched);
    }
    if (unwatched > 0) {
        std::fprintf(stderr,
                     "[comshu rank %d] doorbell hijack: %u UAR(s) FAILED OPEN — the application "
                     "got the real doorbell page and their sends are absent from this "
                     "capture.\n",
                     rank, unwatched);
    }
}

}  // namespace mlx5::hijack
