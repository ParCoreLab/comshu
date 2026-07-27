// NVBit backend for the gpumem provider — port of snoopie's mem_multigpu.cu,
// reduced to the capture path and re-targeted at pcomm::gpumem::Engine:
//
//   nvbit_at_cuda_event  -> instrument mem ops, track allocations
//   instrumentation pass -> Engine::on_source_line (SourceLine artifacts)
//   alloc callbacks      -> Engine::on_alloc (MemObject entities + registry)
//   recv thread          -> pcomm_gpu_access_t -> RawAccess -> Engine::process
//
// Not yet ported from snoopie (phase-1 follow-ups): NVSHMEM heap ownership
// (2.x range + 3.x cuMemMap regions), sampling, python/NCCL attribution,
// data-object callstack attribution (adm), cuMemcpyDtoD synthesis.
//
// Vendor types (CUcontext, Instr, ...) are allowed HERE and nowhere above the
// gpu-backends/ boundary.

#include <algorithm>
#include <cassert>
#include <cstdint>
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

#define CHANNEL_SIZE (1l << 30)

namespace {

struct CtxState {
    ChannelDev* channel_dev{nullptr};
    ChannelHost channel_host;
};

// NVBit drives context teardown from an atexit handler, which can run after
// this .so's C++ static destructors. Every global these callbacks touch is
// therefore heap-allocated and deliberately leaked (or trivially
// destructible) so no exit-ordering can leave a callback with a dead object.
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;  // serializes callbacks

// NVSHMEM 3.x symmetric-heap region (cuMemMap-based). All devices map all
// regions, so ownership is positional: sorted by base, region 0 is the local
// PE and region i belongs to PE (mype + i) % npes (per-process mapping).
struct NvshmemHeapRegion {
    uint64_t base;
    uint64_t size;
    int region_index;
};

struct Globals {
    std::unordered_map<CUcontext, CtxState*> ctx_state;
    std::unordered_set<CUfunction> instrumented;
    std::map<std::string, int> opcode_to_id;
    std::vector<std::string> id_to_opcode;
    std::vector<MemOp> id_to_memop;
    std::string kernel_filter = "all";
    // Written only by the CUDA-callback thread under g_mutex.
    std::vector<NvshmemHeapRegion> heap_regions;
    std::unordered_set<uint64_t> heap_bases_seen;
    // What the recv thread actually reads: an immutable, already-sorted
    // snapshot, published atomically. The reader used to sort `heap_regions` in
    // place on first use while the callback thread could be push_back()ing into
    // it — a reallocation mid-sort is a use-after-free, and it sits in the
    // multinode NVSHMEM path. Readers never mutate now, so there is nothing to
    // race with. Old snapshots are leaked on purpose: they are a few dozen bytes
    // and republishing happens only when a new symmetric-heap region appears, so
    // reclaiming them would need hazard pointers to be safe against a reader
    // that is mid-iteration.
    std::atomic<const std::vector<NvshmemHeapRegion>*> heap_snapshot{nullptr};
    std::string nvshmem_version = "2.8";  // legacy-path layout selector
    int nvshmem_ngpus = 10;
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

// Publish a fresh sorted snapshot for readers. Caller must hold g_mutex.
void publish_heap_snapshot() {
    auto& g = G();
    auto* snap = new std::vector<NvshmemHeapRegion>(g.heap_regions);
    std::sort(snap->begin(), snap->end(),
              [](const NvshmemHeapRegion& a, const NvshmemHeapRegion& b) {
                  return a.base < b.base;
              });
    for (int i = 0; i < (int)snap->size(); i++) (*snap)[i].region_index = i;
    g.heap_snapshot.store(snap, std::memory_order_release);
}

// NVSHMEM 3.x path: region index -> PE (mype + i) % npes. -1 = not heap.
// Read-only and lock-free: called per access from the recv thread, so taking
// g_mutex here would serialize the drain path against every CUDA callback.
int find_nvshmem_heap_owner(uint64_t addr, int mype) {
    const auto* snap = G().heap_snapshot.load(std::memory_order_acquire);
    if (!snap || snap->empty()) return -1;
    const int npes = (int)snap->size();
    for (const auto& r : *snap) {
        if (addr >= r.base && addr < r.base + r.size)
            return (mype + r.region_index) % npes;
    }
    return -1;
}

// NVSHMEM 2.x legacy path: fixed VA layout, peers mapped at 0x12020000000 with
// constant stride (snoopie's find_nvshmem_dev_of_ptr, verbatim logic).
int64_t find_nvshmem_dev_of_ptr_legacy(int mype, uint64_t mem_addr) {
    const int size = 15;
    const uint64_t start = 0x000012020000000;
    const uint64_t incr = start - (uint64_t)0x000010020000000;

    int region = -1;
    for (int i = 1; i <= size; i++) {
        uint64_t bottom = start + (uint64_t)(i - 1) * incr;
        uint64_t top = start + (uint64_t)i * incr;
        if (bottom <= mem_addr && mem_addr < top) {
            region = i - 1;
            break;
        }
    }
    if (region == -1) return -1;

    auto& g = G();
    if (g.nvshmem_version == "2.9" || g.nvshmem_version == "2.8") region += mype;
    if (mype == region) return (mype + 1) % g.nvshmem_ngpus;
    for (int i = 0; i < size; i++) {
        if (mype == i) continue;
        if (region == 0) return i % g.nvshmem_ngpus;
        region--;
    }
    return -1;
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
            if (instr->getMemorySpace() == InstrType::MemorySpace::NONE ||
                instr->getMemorySpace() == InstrType::MemorySpace::CONSTANT) {
                continue;
            }

            auto [it, inserted] = G().opcode_to_id.emplace(instr->getOpcode(),
                                                         (int)G().id_to_opcode.size());
            if (inserted) {
                G().id_to_opcode.emplace_back(instr->getOpcode());
                G().id_to_memop.push_back(opcode_to_memop(instr->getOpcode()));
            }
            int opcode_id = it->second;

            uint32_t instr_index = g_next_instr_index++;

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

    for (auto f : related) {
        std::string func_name(nvbit_get_func_name(ctx, f));
        if (G().kernel_filter == "all" ||
            G().kernel_filter == func_name.substr(0, func_name.find('('))) {
            instrument_function_if_needed(ctx, f);
        }
        nvbit_set_at_launch(ctx, f, g_grid_launch_id);
        g_grid_launch_id++;
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
    {
        MutexLock lock(g_mutex);
        ch_host = &G().ctx_state.at(ctx)->channel_host;
    }

    char* recv_buffer = (char*)malloc(CHANNEL_SIZE);
    std::vector<RawAccess> batch;

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
    unsigned long long recv_calls = 0, empty_calls = 0, total_bytes = 0;
    unsigned long max_n = 0;
    auto t_start = std::chrono::steady_clock::now();
    auto t_last_data = t_start, t_last_report = t_start;

    bool done = false;
    while (!done) {
        uint32_t n = ch_host->recv(recv_buffer, CHANNEL_SIZE);
        ++recv_calls;
        if (n == 0) {
            ++empty_calls;
            if (dbg) {
                auto now = std::chrono::steady_clock::now();
                if (now - t_last_report >= std::chrono::seconds(10)) {
                    t_last_report = now;
                    auto since_data = std::chrono::duration_cast<std::chrono::seconds>(
                                          now - t_last_data).count();
                    std::fprintf(stderr,
                        "[pcomm-chan] ctx=%p idle: recv_calls=%llu empty=%llu "
                        "bytes=%llu max_recv=%lu no_data_for=%llds "
                        "=> doorbell is 0, device is NOT waiting on the host\n",
                        (void*)ctx, recv_calls, empty_calls, total_bytes, max_n,
                        (long long)since_data);
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

                // NVSHMEM symmetric-heap ownership (3.x regions, then 2.x
                // fixed layout); anything else stays unresolved and the
                // engine consults the alloc registry.
                int owner = find_nvshmem_heap_owner(a.address, ma->dev_id);
                if (owner < 0 && a.address >= 0x0000010020000000ull) {
                    owner = (int)find_nvshmem_dev_of_ptr_legacy(ma->dev_id, a.address);
                }
                if (owner >= 0)
                    a.mem_device = static_cast<pcomm::core::DeviceId>(owner);

                batch.push_back(a);
            }
        }

        g_engine->process(batch);
        batch.clear();
    }

    if (dbg)
        std::fprintf(stderr,
            "[pcomm-chan] ctx=%p done: recv_calls=%llu empty=%llu bytes=%llu "
            "max_recv=%lu\n",
            (void*)ctx, recv_calls, empty_calls, total_bytes, max_n);
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
    if (const char* v = std::getenv("PCOMM_NVSHMEM_VERSION")) G().nvshmem_version = v;
    if (const char* v = std::getenv("PCOMM_NVSHMEM_NGPUS")) G().nvshmem_ngpus = std::atoi(v);
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
        // NVSHMEM 3.x symmetric heap: record each distinct mapped base as a
        // region (ownership resolved positionally on the drain path).
        auto* p = (cuMemMap_params*)params;
        auto& g = G();
        if (g.heap_bases_seen.insert((uint64_t)p->ptr).second) {
            g.heap_regions.push_back(
                {(uint64_t)p->ptr, (uint64_t)p->size, (int)g.heap_regions.size()});
            publish_heap_snapshot();  // sorted under g_mutex, read lock-free
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
    state->channel_host.init((int)G().ctx_state.size() - 1, CHANNEL_SIZE,
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
