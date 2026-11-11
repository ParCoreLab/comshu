// nvbit_ldl_wedge — the smallest NVBit tool that instruments ONE local-memory
// load and nothing else, to reproduce the NVSHMEM IBRC hang outside comshu.
//
// The finding it tests (things-to-circle-back-to.md, MN5 jobs
// 43918460..43920181): instrumenting a single LDL.64 (a spilled-register reload,
// index 1006, +0x19680) in nvshmemi_transfer_rma_p<int> makes an NVSHMEM run over
// the IBRC proxy hang, and only when the injected call PUSHES to a channel. If
// this tool reproduces it, the defect is NVBit's or NVSHMEM's, not comshu's.
//
// Environment:
//   WEDGE_MODE    none | empty | atomic | channel | shfl   (default none)
//   WEDGE_KERNEL  instrument launches of kernels whose name contains this
//                 (default put_to_next — case 04/07's workload)
//   WEDGE_FN      ...and, among their related functions, only those whose name
//                 contains this (default nvshmemi_transfer_rma_p)
//   WEDGE_OFF     hex offset of the one instruction; unset = every LOCAL load
//                 in WEDGE_FN
//   WEDGE_DUMP=1  print every LOCAL-space instruction considered, with offsets
//   WEDGE_SPACES  local (default) | all — all adds GLOBAL/GENERIC accesses
#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_set>

#include "nvbit.h"
#include "nvbit_tool.h"
#include "utils/channel.hpp"

static constexpr int kChannelBytes = 1 << 20;

static std::string g_mode, g_kernel, g_fn;
static long        g_off = -1;
static bool        g_dump = false;
static bool        g_all_spaces = false;

// Managed memory allocated at context init, not a `__managed__` global as in
// NVBit's example: the vendored channel.hpp gives ChannelDev a member default
// initializer, and nvcc rejects dynamic initialization of a __managed__ variable.
static ChannelDev*                    g_channel_dev = nullptr;
static ChannelHost                    g_channel_host;
static volatile bool                  g_recv_running = false;
static unsigned long long*            g_counter = nullptr;  // managed
static unsigned long long             g_received = 0;
static std::unordered_set<CUfunction> g_seen;
static int                            g_instrumented = 0;

static void* recv_thread(void*) {
    char* buf = static_cast<char*>(std::malloc(kChannelBytes));
    while (g_recv_running) {
        const uint32_t n = g_channel_host.recv(buf, kChannelBytes);
        g_received += n;  // bytes
    }
    std::free(buf);
    return nullptr;
}

void nvbit_at_init() {
    auto env = [](const char* k, const char* d) {
        const char* v = std::getenv(k);
        return std::string(v && *v ? v : d);
    };
    g_mode = env("WEDGE_MODE", "none");
    g_kernel = env("WEDGE_KERNEL", "put_to_next");
    g_fn = env("WEDGE_FN", "nvshmemi_transfer_rma_p");
    if (const char* o = std::getenv("WEDGE_OFF"); o && *o)
        g_off = std::strtol(o, nullptr, 16);
    g_dump = std::getenv("WEDGE_DUMP") != nullptr;
    g_all_spaces = env("WEDGE_SPACES", "local") == "all";
    std::fprintf(stderr, "[wedge] mode=%s kernel=%s fn=%s off=%s\n", g_mode.c_str(),
                 g_kernel.c_str(), g_fn.c_str(),
                 g_off < 0 ? "all-LOCAL" : std::to_string(g_off).c_str());
}

void nvbit_at_ctx_init(CUcontext) {
    if (g_mode == "channel" || g_mode == "shfl") {
        void* mem = nullptr;
        cudaMallocManaged(&mem, sizeof(ChannelDev));
        g_channel_dev = new (mem) ChannelDev();
        g_recv_running = true;
        g_channel_host.init(0, kChannelBytes, g_channel_dev, recv_thread, nullptr);
    }
    if (g_mode == "atomic") {
        cudaMallocManaged(&g_counter, sizeof(*g_counter));
        *g_counter = 0;
    }
}

static void instrument(CUcontext ctx, CUfunction kernel) {
    std::vector<CUfunction> fns = nvbit_get_related_functions(ctx, kernel);
    fns.push_back(kernel);
    for (CUfunction f : fns) {
        if (!g_seen.insert(f).second)
            continue;
        const char* name = nvbit_get_func_name(ctx, f, false);
        if (!name || std::strstr(name, g_fn.c_str()) == nullptr)
            continue;
        for (Instr* i : nvbit_get_instrs(ctx, f)) {
            // WEDGE_SPACES=all adds every GLOBAL/GENERIC access (each with a
            // memory operand) to the LOCAL loads: the original bisect window
            // [0,1007) instrumented all of them, and the LDL.64 last.
            const auto sp = i->getMemorySpace();
            const bool ldl = sp == InstrType::MemorySpace::LOCAL &&
                             std::strncmp(i->getOpcode(), "LDL", 3) == 0;
            const bool glob = g_all_spaces && (sp == InstrType::MemorySpace::GLOBAL ||
                                               sp == InstrType::MemorySpace::GENERIC);
            if (!ldl && !glob)
                continue;
            bool has_mref = false;
            for (int k = 0; k < i->getNumOperands(); k++)
                has_mref |= i->getOperand(k)->type == InstrType::OperandType::MREF;
            if (!has_mref)
                continue;
            if (g_dump)
                std::fprintf(stderr, "[wedge] %s +0x%x %s\n", name, i->getOffset(), i->getSass());
            if (g_off >= 0 && static_cast<long>(i->getOffset()) != g_off)
                continue;
            if (g_mode == "empty") {
                nvbit_insert_call(i, "wedge_empty", IPOINT_BEFORE);
                nvbit_add_call_arg_guard_pred_val(i);
            } else if (g_mode == "atomic") {
                nvbit_insert_call(i, "wedge_atomic", IPOINT_BEFORE);
                nvbit_add_call_arg_guard_pred_val(i);
                nvbit_add_call_arg_const_val64(i, reinterpret_cast<uint64_t>(g_counter));
            } else if (g_mode == "channel") {
                nvbit_insert_call(i, "wedge_channel", IPOINT_BEFORE);
                nvbit_add_call_arg_guard_pred_val(i);
                nvbit_add_call_arg_const_val64(i, reinterpret_cast<uint64_t>(g_channel_dev));
            } else if (g_mode == "shfl") {
                nvbit_insert_call(i, "wedge_shfl", IPOINT_BEFORE);
                nvbit_add_call_arg_guard_pred_val(i);
                nvbit_add_call_arg_mref_addr64(i, 0);
                nvbit_add_call_arg_const_val64(i, reinterpret_cast<uint64_t>(g_channel_dev));
            } else {
                continue;  // none: considered, not instrumented
            }
            ++g_instrumented;
            std::fprintf(stderr, "[wedge] instrumented %s +0x%x %s\n", name, i->getOffset(),
                         i->getSass());
        }
    }
}

void nvbit_at_cuda_event(CUcontext ctx, int is_exit, nvbit_api_cuda_t cbid, const char*,
                         void* params, CUresult*) {
    if (is_exit)
        return;
    CUfunction f = nullptr;
    if (cbid == API_CUDA_cuLaunchKernel || cbid == API_CUDA_cuLaunchKernel_ptsz)
        f = static_cast<cuLaunchKernel_params*>(params)->f;
    else if (cbid == API_CUDA_cuLaunchKernelEx || cbid == API_CUDA_cuLaunchKernelEx_ptsz)
        f = static_cast<cuLaunchKernelEx_params*>(params)->f;
    else if (cbid == API_CUDA_cuLaunchCooperativeKernel ||
             cbid == API_CUDA_cuLaunchCooperativeKernel_ptsz)
        f = static_cast<cuLaunchCooperativeKernel_params*>(params)->f;
    if (!f)
        return;
    const char* kname = nvbit_get_func_name(ctx, f, false);
    const bool  target = kname && std::strstr(kname, g_kernel.c_str()) != nullptr;
    if (target)
        instrument(ctx, f);
    nvbit_enable_instrumented(ctx, f, target, true);
}

void nvbit_at_term() {
    std::fprintf(stderr, "[wedge] done: mode=%s instrumented=%d atomics=%llu channel_bytes=%llu\n",
                 g_mode.c_str(), g_instrumented, g_counter ? *g_counter : 0ull, g_received);
    g_recv_running = false;
}
