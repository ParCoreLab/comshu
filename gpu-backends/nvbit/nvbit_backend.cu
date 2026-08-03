// NVBit backend for the gpumem provider — port of snoopie's mem_multigpu.cu,
// reduced to the capture path and re-targeted at pcomm::gpumem::Engine:
//
//   nvbit_at_cuda_event  -> instrument mem ops, track allocations
//   instrumentation pass -> Engine::on_source_line (SourceLine artifacts)
//   alloc callbacks      -> Engine::on_alloc (MemObject entities + registry)
//   recv thread          -> pcomm_gpu_access_t -> RawAccess -> Engine::process
//
// Not yet ported from snoopie (phase-1 follow-ups): sampling, python/NCCL
// attribution, data-object callstack attribution (adm), cuMemcpyDtoD synthesis.
//
// Vendor types (CUcontext, Instr, ...) are allowed HERE and nowhere above the
// gpu-backends/ boundary.

#include <cassert>
#include <cstdint>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nvbit.h"
#include "nvbit_tool.h"
#include <atomic>
#include <chrono>

#include "utils/channel.hpp"

#include "dev_common.h"
#include "engine.hpp"

using pcomm::attr::RawAccess;
using pcomm::core::MemOp;

// Ring size per CUDA context, host-pinned. Overridable with PCOMM_CHANNEL_SIZE
// (bytes) — needed to test whether the case 07 hang tracks the ring size, and
// useful anyway: 1 GiB *per context* is a lot to pin on a 4-GPU node, and the
// recv thread mallocs the same amount again for its staging buffer.
#define CHANNEL_SIZE_DEFAULT (1l << 30)
static long channel_size() {
    static long sz = [] {
        if (const char* e = std::getenv("PCOMM_CHANNEL_SIZE")) {
            long v = std::atol(e);
            if (v >= (1l << 20)) return v;   // sanity floor: 1 MiB
        }
        return (long)CHANNEL_SIZE_DEFAULT;
    }();
    return sz;
}

namespace {

struct CtxState {
    ChannelDev* channel_dev{nullptr};
    ChannelHost channel_host;
    volatile unsigned long long* dbg_host{nullptr};  // PCOMM_CHANNEL_DEBUG counters
};

// NVBit drives context teardown from an atexit handler, which can run after
// this .so's C++ static destructors. Every global these callbacks touch is
// therefore heap-allocated and deliberately leaked (or trivially
// destructible) so no exit-ordering can leave a callback with a dead object.
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;  // serializes callbacks

// A region some library mapped with cuMemMap, as observed — base and size, no
// interpretation. NVSHMEM's symmetric heap arrives this way; so do other
// things, and telling them apart is nvshmem_ptr's job, not ours (see
// refresh_mapped_regions).
struct MappedRegion {
    uint64_t base;
    uint64_t size;
};

struct Globals {
    std::unordered_map<CUcontext, CtxState*> ctx_state;
    std::unordered_set<CUfunction> instrumented;
    std::map<std::string, int> opcode_to_id;
    std::vector<std::string> id_to_opcode;
    std::vector<MemOp> id_to_memop;
    std::string kernel_filter = "all";
    // Written only by the CUDA-callback thread under g_mutex.
    std::vector<MappedRegion> mapped_regions;
    std::unordered_set<uint64_t> mapped_bases_seen;
    // What the recv thread actually reads: an immutable snapshot, published
    // atomically. The reader used to sort `mapped_regions` in place on first
    // use while the callback thread could be push_back()ing into it — a
    // reallocation mid-sort is a use-after-free, and it sits in the multinode
    // NVSHMEM path. Readers never mutate now, so there is nothing to race with.
    // Old snapshots are leaked on purpose: they are a few dozen bytes and
    // republishing happens only when a new mapping appears, so reclaiming them
    // would need hazard pointers to be safe against a reader that is
    // mid-iteration.
    std::atomic<const std::vector<MappedRegion>*> mapped_snapshot{nullptr};
};
Globals& G() {
    static Globals* g = new Globals();
    return *g;
}

pcomm::gpumem::Engine* g_engine = nullptr;
bool g_skip_callback = false;
bool g_shutdown_done = false;
uint32_t g_next_instr_index = 0;
uint64_t g_grid_launch_id = 0;

struct MutexLock {
    explicit MutexLock(pthread_mutex_t& m) : m_m(&m) { pthread_mutex_lock(m_m); }
    ~MutexLock() { pthread_mutex_unlock(m_m); }
    pthread_mutex_t* m_m;
};

// Publish a fresh snapshot for readers. Caller must hold g_mutex.
void publish_mapped_snapshot() {
    auto& g = G();
    g.mapped_snapshot.store(new std::vector<MappedRegion>(g.mapped_regions),
                            std::memory_order_release);
}

// --- Ownership of cuMemMap'd memory, NVSHMEM's symmetric heap included -------
//
// This used to be derived from NVSHMEM's *private* VA layout: sort the cuMemMap
// regions, and region i belongs to PE (mype + i) % npes with npes taken from
// how many regions had been seen. Every part of that was wrong. NVSHMEM maps
// ~76 regions for 8 PEs, so npes was wrong; the index was positional, so the
// answer depended on how many regions existed at the moment a record happened
// to be drained; and ownership is not obliged to be contiguous or rotational.
// Measured mem_dev = 64,65,66,67 where the truth is 1,2,3 (job 44111484 vs
// 44113361), which RemoteFilter (run_device != mem_device) then turned into
// phantom remote traffic — case 07 doubled, 196608 -> 393216 accesses per rank.
// A 2.x sibling path hardcoded a base VA and a stride outright.
//
// So we ask NVSHMEM instead. nvshmem_ptr(a, pe) returns the local address of PE
// pe's copy of symmetric object a, or NULL when pe is not P2P-reachable — which
// is precisely the map the old code reconstructed. Resolved with dlsym at
// runtime, so the provider keeps no build or link dependency on NVSHMEM: no
// symbols means the application is not using it and the path is skipped.
//
// Validated on MN5 (NVSHMEM 3.2.5, jobs 44135828/44136107) *before* being built
// on, in four configurations — 4 PEs on one node, 8 PEs over two nodes with
// IBRC and with IBGDA, and NVSHMEM_DISABLE_P2P:
//   - nvshmem_ptr(a, my_pe) == a for every local symmetric address probed:
//     allocation bases, interiors, and a 64 MiB object's last byte;
//   - NULL for everything that is *not* local symmetric memory — cudaMalloc
//     pointers, host addresses, junk VAs, and peers' mappings of the heap — so
//     "non-NULL for my_pe" is a sound test for "this region is my heap", with
//     no false positives to mis-expand;
//   - non-NULL exactly for the P2P-reachable PEs: 0-3 within a node, NULL for
//     the four PEs across the IB link (which is why those ranks legitimately
//     record no NVLink traffic), and self only under NVSHMEM_DISABLE_P2P;
//   - callable from a non-main thread, which the recv thread is.
struct NvshmemApi {
    void* (*ptr)(const void*, int){nullptr};
    int (*my_pe)(){nullptr};
    int (*n_pes)(){nullptr};
    int (*init_status)(){nullptr};
    bool ok() const { return ptr && my_pe && n_pes && init_status; }
};

// From nvshmem_constants.h; not included, since the whole point is to have no
// build dependency on NVSHMEM.
constexpr int kNvshmemStatusIsInitialized = 2;

// Lazy on purpose: NVSHMEM is not loaded when the tool's nvbit_at_init runs, so
// resolving there would fail permanently. Called only under g_heap_mutex.
const NvshmemApi& nvshmem_api() {
    static NvshmemApi* api = new NvshmemApi();  // leaked: exit-order safe
    if (!api->ok()) {
        api->ptr = (void* (*)(const void*, int))dlsym(RTLD_DEFAULT, "nvshmem_ptr");
        api->my_pe = (int (*)())dlsym(RTLD_DEFAULT, "nvshmem_my_pe");
        api->n_pes = (int (*)())dlsym(RTLD_DEFAULT, "nvshmem_n_pes");
        api->init_status = (int (*)())dlsym(RTLD_DEFAULT, "nvshmemx_init_status");
    }
    return *api;
}

// NOT g_mutex: nvbit_at_ctx_term holds that across the pthread_join of the recv
// thread, so anything the recv thread locks must be a different mutex or
// teardown deadlocks. Guards nvshmem_api()'s resolution and the resolved sets.
std::mutex g_heap_mutex;
std::unordered_set<uint64_t>* g_mapped_resolved = new std::unordered_set<uint64_t>();
std::unordered_set<uint64_t>* g_heap_resolved = new std::unordered_set<uint64_t>();

// Which GPU does the memory at `p` live on? CUDA's own answer, in the same
// space run_device is in. Negative when it cannot say.
int device_of(uint64_t p, int ndev, const char* what) {
    int dev = -1;
    if (cuPointerGetAttribute(&dev, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL,
                              (CUdeviceptr)p) != CUDA_SUCCESS)
        return -1;
    // The guard the old scheme could not have had: an owner must name a GPU
    // that exists. mem_dev = 64 on a 4-GPU node would have stopped here,
    // loudly, instead of quietly inflating the capture.
    if (dev < 0 || dev >= ndev) {
        std::fprintf(stderr, "[pcomm-gpumem] %s owner out of range: dev=%d of %d "
                     "GPUs; 0x%llx dropped\n", what, dev, ndev,
                     (unsigned long long)p);
        return -1;
    }
    return dev;
}

// Turn newly observed cuMemMap regions into (range -> device) entries in the
// alloc registry the drain path already consults. Nothing vendor-shaped
// survives on the hot path: attribution is one range lookup, as it is for
// cudaMalloc'd memory. Called from the recv thread when a new snapshot appears.
void refresh_mapped_regions(const std::vector<MappedRegion>* snap) {
    if (!snap || snap->empty() || !g_engine) return;
    std::lock_guard<std::mutex> lock(g_heap_mutex);

    // CUDA calls are safe from the recv thread despite g_mutex:
    // nvbit_at_ctx_init registers it with nvbit_set_tool_pthread, so NVBit does
    // not deliver callbacks for the tool's own driver calls and there is no
    // re-entry into nvbit_at_cuda_event to block on.
    int ndev = 0;
    cudaGetDeviceCount(&ndev);

    // 1. Every mapped region, from CUDA. cuMemMap is not NVSHMEM's alone —
    //    NCCL registers its peer buffers through the same APIs, and the
    //    cuMemAlloc-fed registry never sees those, so without this they resolve
    //    to run_device and RemoteFilter drops real peer traffic. Case 05's
    //    peer traffic used to appear only because the NVSHMEM positional guess
    //    was being applied to NCCL's regions — it reported mem_dev up to 8 on a
    //    4-GPU node, i.e. it was fabricating the answer that made the case pass.
    for (const auto& r : *snap) {
        if (!g_mapped_resolved->insert(r.base).second) continue;
        const int dev = device_of(r.base, ndev, "mapped region");
        if (dev >= 0)
            g_engine->allocs().add_range(r.base, r.size,
                                         static_cast<pcomm::core::DeviceId>(dev));
    }

    // 2. NVSHMEM's symmetric heap, authoritatively. Step 1 already covers every
    //    peer mapping we *observed* a cuMemMap for; this covers the rest and is
    //    the only source that knows a region is symmetric memory at all rather
    //    than inferring it from where it happens to sit in the VA space.
    const NvshmemApi& api = nvshmem_api();
    if (!api.ok()) {
        // Not an error: most workloads are not NVSHMEM. Say it once under
        // PCOMM_VERBOSE rather than either staying silent or annotating every
        // run that was never going to have a symmetric heap.
        static bool said = false;
        if (!said && std::getenv("PCOMM_VERBOSE")) {
            said = true;
            std::fprintf(stderr, "[pcomm-gpumem] no NVSHMEM symbols; mapped regions "
                         "attributed from CUDA alone\n");
        }
        return;  // retried on the next snapshot — NVSHMEM may load later
    }
    // nvshmem_ptr dereferences NVSHMEM's heap state, which does not exist before
    // nvshmem_init or after nvshmem_finalize; calling it outside that window
    // segfaults on a null+0x20 (all four ranks of case 04, job 44136544 — the
    // recv thread's last refresh lands after the application has finalized).
    // nvshmem_my_pe and nvshmem_n_pes keep answering plausibly across the same
    // boundary, so they cannot be the test. nvshmemx_init_status can, and is
    // required rather than optional: without it there is no safe window to
    // check and the heap path must stay off.
    if (api.init_status() < kNvshmemStatusIsInitialized) return;

    const int npes = api.n_pes();
    const int mype = api.my_pe();
    if (npes <= 0 || mype < 0 || mype >= npes) return;

    for (const auto& r : *snap) {
        if (g_heap_resolved->count(r.base)) continue;
        // Is this region *my* symmetric heap? Only then do we know what its
        // peers' copies are. A region that is not (yet) mine is left for a
        // later refresh: NVSHMEM maps the heap progressively, and unlike the
        // positional scheme, being early costs nothing here — the answer for a
        // region never depends on how many others have been seen.
        if (!api.ptr((const void*)r.base, mype)) continue;
        g_heap_resolved->insert(r.base);

        for (int pe = 0; pe < npes; pe++) {
            void* q = api.ptr((const void*)r.base, pe);
            if (!q) continue;  // not P2P-reachable: that PE's traffic crosses
                               // the IB link and belongs to the ib provider.
            // PE is not a device. The old code cast one straight to the other,
            // which is only accidentally right when PEs and GPUs are numbered
            // alike on one node. CUDA knows where the memory nvshmem_ptr just
            // named actually lives (measured, job 44136107), so ask it rather
            // than inherit the conflation.
            const int dev = device_of((uint64_t)q, ndev, "NVSHMEM heap");
            if (dev >= 0)
                g_engine->allocs().add_range((uint64_t)q, r.size,
                                             static_cast<pcomm::core::DeviceId>(dev));
        }
    }
}

MemOp opcode_to_memop(const std::string& opcode) {
    if (opcode.rfind("LD", 0) == 0) return MemOp::Load;
    if (opcode.rfind("ST", 0) == 0) return MemOp::Store;
    if (opcode.rfind("ATOM", 0) == 0 || opcode.rfind("RED", 0) == 0)
        return MemOp::Atomic;
    return MemOp::Load;
}

void instrument_function_if_needed(CUcontext ctx, CUfunction func) {
    std::vector<CUfunction> related = nvbit_get_related_functions(ctx, func);
    related.push_back(func);

    for (auto f : related) {
        if (!G().instrumented.insert(f).second) continue;

        int dev_id = -1;
        cudaGetDevice(&dev_id);

        const std::vector<Instr*>& instrs = nvbit_get_instrs(ctx, f);
        for (auto instr : instrs) {
            // Only GLOBAL (and GENERIC, which resolves to global at run time)
            // can be communication. LOCAL is per-thread stack and spilled
            // registers; SHARED is intra-block. Instrumenting them produced
            // records that are meaningless for a communication trace, and it
            // is the majority of injected calls: in one 64-instruction window
            // of nvshmemi_transfer_rma_p<int>, 84 of the accesses were
            // LDS/LDL/STL.
            //
            // It is also the case 07 hang. Bisected to a single instruction
            // (job 43920181): instrumenting index 1006, an LDL.64 at
            // +0x19680 inside nvshmemi_transfer_rma_p<int>, flips the run from
            // completing in 22s to hanging. Instrumenting everything below it,
            // including the 64-bit global atomic and the STRONG.SYS stores
            // around it, completes.
            {
                const auto sp = instr->getMemorySpace();
                if (sp != InstrType::MemorySpace::GLOBAL &&
                    sp != InstrType::MemorySpace::GENERIC &&
                    sp != InstrType::MemorySpace::GLOBAL_TO_SHARED) {
                    continue;
                }
            }

            auto [it, inserted] = G().opcode_to_id.emplace(instr->getOpcode(),
                                                         (int)G().id_to_opcode.size());
            if (inserted) {
                G().id_to_opcode.emplace_back(instr->getOpcode());
                G().id_to_memop.push_back(opcode_to_memop(instr->getOpcode()));
            }
            int opcode_id = it->second;

            uint32_t instr_index = g_next_instr_index++;

            // PCOMM_INSTR_LO/HI: instrument only instructions whose dense index
            // falls in [LO, HI). Diagnostic for case 07 — the hang needs push()
            // to run, so bisecting the instrumented set names the instruction
            // whose records trigger it. Indices are assigned in a fixed order
            // (module load order, then instruction offset), so a given index
            // denotes the same instruction across runs of the same binary.
            // Unset = instrument everything, the normal path.
            {
                static const uint32_t lo = [] {
                    const char* e = std::getenv("PCOMM_INSTR_LO");
                    return e ? (uint32_t)std::strtoul(e, nullptr, 10) : 0u;
                }();
                static const uint32_t hi = [] {
                    const char* e = std::getenv("PCOMM_INSTR_HI");
                    return e ? (uint32_t)std::strtoul(e, nullptr, 10) : UINT32_MAX;
                }();
                if (instr_index < lo || instr_index >= hi) continue;
            }

            // PCOMM_INSTR_DUMP: print the index -> instruction mapping so a
            // bisect result (an index) can be turned back into an opcode, a
            // function and an offset. Diagnostic only.
            if (std::getenv("PCOMM_INSTR_DUMP")) {
                const char* fn = nvbit_get_func_name(ctx, f, true);
                std::fprintf(stderr,
                    "[pcomm-instr] idx=%u opcode=%s space=%d size=%d off=0x%x fn=%s\n",
                    instr_index, instr->getOpcode(),
                    (int)instr->getMemorySpace(), instr->getSize(),
                    (unsigned)instr->getOffset(), fn ? fn : "?");
            }

            // Source-line artifact (nvbit_get_line_info needs -lineinfo in the
            // target app; silently absent otherwise, like snoopie).
            char* file_name = nullptr;
            char* dir_name = nullptr;
            uint32_t line = 0;
            if (nvbit_get_line_info(ctx, f, instr->getOffset(), &file_name,
                                    &dir_name, &line) && line != 0) {
                g_engine->on_source_line(instr_index, file_name ? file_name : "",
                                         dir_name ? dir_name : "", line);
            }

            int mref_idx = 0;
            for (int i = 0; i < instr->getNumOperands(); i++) {
                const InstrType::operand_t* op = instr->getOperand(i);
                if (op->type != InstrType::OperandType::MREF) continue;

                nvbit_insert_call(instr, "pcomm_instrument_mem", IPOINT_BEFORE);
                nvbit_add_call_arg_guard_pred_val(instr);
                nvbit_add_call_arg_const_val32(instr, opcode_id);
                nvbit_add_call_arg_const_val32(instr, dev_id);
                nvbit_add_call_arg_mref_addr64(instr, mref_idx);
                nvbit_add_call_arg_launch_val64(instr, 0);  // grid launch id
                nvbit_add_call_arg_const_val64(
                    instr, (uint64_t)G().ctx_state[ctx]->channel_dev);
                nvbit_add_call_arg_const_val32(instr, (int)instr_index);
                nvbit_add_call_arg_const_val32(instr, (int)instr->getSize());
                mref_idx++;
            }
        }
    }
}

void handle_launch(CUcontext ctx, CUfunction func) {
    std::vector<CUfunction> related = nvbit_get_related_functions(ctx, func);
    related.push_back(func);

    // PCOMM_LAUNCH_DUMP: report the kernel actually being launched, in exactly
    // the form the filter compares against, once per distinct name. This is the
    // input PCOMM_KERNEL_NAME has always needed and had no way to discover —
    // dumping every *instrumented* function instead gives a list that is both
    // enormous (NCCL ships 223 specializations) and truncated by any timeout.
    // Pair it with a filter that matches nothing to skip instrumentation
    // entirely, so the run finishes in seconds.
    if (std::getenv("PCOMM_LAUNCH_DUMP")) {
        static std::unordered_set<std::string>* seen =
            new std::unordered_set<std::string>();
        std::string launched(nvbit_get_func_name(ctx, func));
        launched = launched.substr(0, launched.find('('));
        if (seen->insert(launched).second)
            std::fprintf(stderr, "[pcomm-launch] %s\n", launched.c_str());
    }

    for (auto f : related) {
        std::string func_name(nvbit_get_func_name(ctx, f));
        const bool want = (G().kernel_filter == "all" ||
                           G().kernel_filter ==
                               func_name.substr(0, func_name.find('(')));
        if (want) instrument_function_if_needed(ctx, f);

        // The launch id is consumed for every related function whether or not
        // we instrument it, so that filtering cannot shift the id sequence and
        // move existing goldens.
        const uint64_t launch_id = g_grid_launch_id++;

        // NB: these run for every related function, not just instrumented ones.
        // Skipping them for unfiltered functions was measured (job 43971039)
        // and changes nothing — 436s vs 436s, 438s vs 438s — so the knob was
        // removed rather than left as a dead option. See case 05's meta.json:
        // the cost is NVBit module processing, which we cannot reach from here.
        nvbit_set_at_launch(ctx, f, launch_id);
        nvbit_enable_instrumented(ctx, f, true);
    }
}

void handle_alloc(uint64_t base, uint64_t size) {
    int dev = -1;
    cudaGetDevice(&dev);
    g_engine->on_alloc(base, size, static_cast<pcomm::core::DeviceId>(dev));
}

void* recv_thread_fun(void* args) {
    CUcontext ctx = (CUcontext)args;

    ChannelHost* ch_host;
    // Grab the debug counters here too, while we already hold the lock and
    // while the thread is guaranteed to be joinable. Do NOT take g_mutex again
    // later in this function: nvbit_at_ctx_term holds it across the
    // pthread_join of this very thread, so any exit-path lock deadlocks at
    // teardown for every workload size. dbg_host is assigned in
    // nvbit_at_ctx_init before channel_host.init() spawns us, so it is already
    // visible.
    const volatile unsigned long long* dbg_ctrs = nullptr;
    {
        MutexLock lock(g_mutex);
        CtxState* st = G().ctx_state.at(ctx);
        ch_host = &st->channel_host;
        dbg_ctrs = st->dbg_host;
    }

    const long chan_sz = channel_size();
    char* recv_buffer = (char*)malloc(chan_sz);
    std::vector<RawAccess> batch;
    const std::vector<MappedRegion>* heap_snap_seen = nullptr;

    // PCOMM_CHANNEL_DEBUG=1 — diagnostic for the NVBit deadlock (cases 05/07,
    // plans/PLAN-20260727-1022-nvbit-channel-deadlock-backlog.md). The decisive
    // question is whether the device is stalled waiting on us.
    //
    // ChannelDev::flush() sets the doorbell and then spins *inside the kernel*
    // until the host clears it, and recv() returns >0 exactly when the doorbell
    // is non-zero. So the two states are distinguishable from here alone, with
    // no change to the vendored channel.hpp:
    //   recv() keeps returning 0 while hung  -> device never flushed; the ring
    //                                           is NOT full and the channel is
    //                                           not what is blocking.
    //   recv() returns data then we stall    -> we are the bottleneck; look at
    //                                           process()/the drain path.
    const bool dbg = std::getenv("PCOMM_CHANNEL_DEBUG") != nullptr;
    if (dbg)
        std::fprintf(stderr, "[pcomm-chan] ctx=%p ring=%ld bytes\n", (void*)ctx, chan_sz);
    unsigned long long recv_calls = 0, empty_calls = 0, total_bytes = 0;
    unsigned long max_n = 0;
    auto t_start = std::chrono::steady_clock::now();
    auto t_last_data = t_start, t_last_report = t_start;

    bool done = false;
    while (!done) {
        uint32_t n = ch_host->recv(recv_buffer, chan_sz);
        ++recv_calls;
        if (n == 0) {
            ++empty_calls;
            if (dbg) {
                auto now = std::chrono::steady_clock::now();
                if (now - t_last_report >= std::chrono::seconds(10)) {
                    t_last_report = now;
                    auto since_data = std::chrono::duration_cast<std::chrono::seconds>(
                                          now - t_last_data).count();
                    const volatile unsigned long long* d = dbg_ctrs;
                    std::fprintf(stderr,
                        "[pcomm-chan] ctx=%p idle: bytes=%llu no_data_for=%llds "
                        "| dev push=%llu full=%llu flusher=%llu waiter=%llu "
                        "flushbody=%llu ring=%llu cleared=%llu done=%llu\n",
                        (void*)ctx, total_bytes, (long long)since_data,
                        d ? d[0] : 0ull, d ? d[1] : 0ull, d ? d[2] : 0ull, d ? d[3] : 0ull,
                        d ? d[4] : 0ull, d ? d[5] : 0ull, d ? d[6] : 0ull, d ? d[7] : 0ull);
                }
            }
            continue;
        }
        total_bytes += n;
        if (n > max_n) max_n = n;
        t_last_data = std::chrono::steady_clock::now();

        for (uint32_t off = 0; off + sizeof(pcomm_gpu_access_t) <= n;
             off += sizeof(pcomm_gpu_access_t)) {
            auto* ma = (pcomm_gpu_access_t*)&recv_buffer[off];
            if (ma->lane_id == -1) {  // flush token: context is terminating
                done = true;
                break;
            }

            for (int lane = 0; lane < 32; lane++) {
                if (ma->addrs[lane] == 0) continue;
                RawAccess a;
                a.address = ma->addrs[lane];
                a.pc = pcomm::gpumem::pc_handle(ma->instr_index);
                a.size = ma->access_size;
                a.thread = static_cast<uint32_t>(ma->thread_index) + lane;
                a.op = G().id_to_memop[ma->opcode_id];
                a.space = pcomm::core::MemSpace::Global;
                a.run_device = static_cast<pcomm::core::DeviceId>(ma->dev_id);
                // mem_device is left unresolved: symmetric-heap ranges are in
                // the alloc registry like any other allocation, so the engine
                // resolves every address the same way.
                batch.push_back(a);
            }
        }

        // Pick up regions mapped since the last batch. One atomic load unless a
        // new mapping actually appeared. Not once `done`: that token means the
        // context is terminating, so the application has already finalized
        // NVSHMEM and probing it would be a use-after-free (see
        // refresh_mapped_regions). The records already in hand still drain.
        const auto* snap = G().mapped_snapshot.load(std::memory_order_acquire);
        if (!done && snap != heap_snap_seen) {
            heap_snap_seen = snap;
            refresh_mapped_regions(snap);
        }

        g_engine->process(batch);
        batch.clear();
    }

    if (dbg) {
        // Device counters on the done line too, not just the idle reports —
        // otherwise a *completing* run reports no push total, and there is
        // nothing to compare a hung run's frozen count against.
        const volatile unsigned long long* d = dbg_ctrs;
        std::fprintf(stderr,
            "[pcomm-chan] ctx=%p done: recv_calls=%llu empty=%llu bytes=%llu "
            "max_recv=%lu%s",
            (void*)ctx, recv_calls, empty_calls, total_bytes, max_n,
            d ? "" : "\n");
        if (d)
            std::fprintf(stderr,
                " | dev push=%llu full=%llu flusher=%llu waiter=%llu "
                "flushbody=%llu ring=%llu cleared=%llu done=%llu\n",
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    g_engine->drain_thread();  // buffers never auto-drain (leaked TLS)
    free(recv_buffer);
    return nullptr;
}

}  // namespace

__global__ void pcomm_flush_channel(ChannelDev* ch_dev) {
    pcomm_gpu_access_t ma;
    memset(&ma, 0, sizeof(ma));
    ma.lane_id = -1;
    ch_dev->push(&ma, sizeof(pcomm_gpu_access_t));
    ch_dev->flush();
}

// --- NVBit entry points ------------------------------------------------------

void nvbit_at_init() {
    setenv("CUDA_MANAGED_FORCE_DEVICE_ALLOC", "1", 1);
    if (const char* k = std::getenv("PCOMM_KERNEL_NAME")) G().kernel_filter = k;
    g_engine = new pcomm::gpumem::Engine();
}

void nvbit_tool_init(CUcontext) {}
void nvbit_at_graph_node_launch(CUcontext, CUfunction, CUstream, uint64_t) {}

void nvbit_at_cuda_event(CUcontext ctx, int is_exit, nvbit_api_cuda_t cbid,
                         const char* /*name*/, void* params, CUresult*) {
    MutexLock lock(g_mutex);
    if (g_skip_callback) return;
    g_skip_callback = true;

    if (!is_exit && (cbid == API_CUDA_cuLaunchKernel_ptsz ||
                     cbid == API_CUDA_cuLaunchKernel)) {
        auto* p = (cuLaunchKernel_params*)params;
        handle_launch(ctx, p->f);
    } else if (!is_exit && cbid == API_CUDA_cuLaunchKernelEx) {
        auto* p = (cuLaunchKernelEx_params*)params;
        handle_launch(ctx, p->f);
    } else if (!is_exit && (cbid == API_CUDA_cuLaunchCooperativeKernel ||
                            cbid == API_CUDA_cuLaunchCooperativeKernel_ptsz)) {
        auto* p = (cuLaunchCooperativeKernel_params*)params;
        handle_launch(ctx, p->f);
    } else if (is_exit && cbid == API_CUDA_cuMemAlloc_v2) {
        auto* p = (cuMemAlloc_v2_params*)params;
        handle_alloc(*p->dptr, p->bytesize);
    } else if (is_exit && cbid == API_CUDA_cuMemAlloc) {
        auto* p = (cuMemAlloc_params*)params;
        handle_alloc(*p->dptr, p->bytesize);
    } else if (is_exit && cbid == API_CUDA_cuMemAllocManaged) {
        auto* p = (cuMemAllocManaged_params*)params;
        handle_alloc(*p->dptr, p->bytesize);
    } else if (is_exit && cbid == API_CUDA_cuMemMap) {
        // Record the extent, nothing more. Which of these are NVSHMEM's
        // symmetric heap, and whose, is decided later by asking NVSHMEM — see
        // refresh_mapped_regions. cuMemMap is only the source of exact sizes.
        auto* p = (cuMemMap_params*)params;
        auto& g = G();
        if (g.mapped_bases_seen.insert((uint64_t)p->ptr).second) {
            g.mapped_regions.push_back({(uint64_t)p->ptr, (uint64_t)p->size});
            publish_mapped_snapshot();  // built under g_mutex, read lock-free
        }
    }

    g_skip_callback = false;
}

void nvbit_at_ctx_init(CUcontext ctx) {
    MutexLock lock(g_mutex);
    CtxState* state = new CtxState;
    assert(G().ctx_state.find(ctx) == G().ctx_state.end());
    G().ctx_state[ctx] = state;
    cudaMallocManaged(&state->channel_dev, sizeof(ChannelDev));
    // Diagnostic counters (PCOMM_CHANNEL_DEBUG): host-mapped so the recv thread
    // can watch where the device stops. See channel.hpp pcomm_bump slots.
    if (std::getenv("PCOMM_CHANNEL_DEBUG")) {
        unsigned long long* h = nullptr;
        if (cudaHostAlloc((void**)&h, 8 * sizeof(unsigned long long),
                          cudaHostAllocMapped) == cudaSuccess) {
            memset(h, 0, 8 * sizeof(unsigned long long));
            unsigned long long* d = nullptr;
            if (cudaHostGetDevicePointer((void**)&d, h, 0) == cudaSuccess) {
                state->dbg_host = h;
                state->channel_dev->pcomm_dbg = d;
            }
        }
    }
    state->channel_host.init((int)G().ctx_state.size() - 1, channel_size(),
                             state->channel_dev, recv_thread_fun, ctx);
    nvbit_set_tool_pthread(state->channel_host.get_thread());
}

void nvbit_at_ctx_term(CUcontext ctx) {
    MutexLock lock(g_mutex);
    g_skip_callback = true;
    CtxState* state = G().ctx_state.at(ctx);

    pcomm_flush_channel<<<1, 1>>>(state->channel_dev);
    cudaDeviceSynchronize();
    assert(cudaGetLastError() == cudaSuccess);

    state->channel_host.destroy(false);
    cudaFree(state->channel_dev);
    g_skip_callback = false;
    delete state;
    G().ctx_state.erase(ctx);

    // Last context gone: flush everything now, while the C++ runtime (Tracer,
    // sinks, intern tables) is still alive. nvbit_at_term is too late.
    if (G().ctx_state.empty() && g_engine && !g_shutdown_done) {
        g_shutdown_done = true;
        g_engine->shutdown();
    }
}

void nvbit_at_term() {
    // Runs during __cxa_finalize — statics may be gone; nothing safe to do.
}
