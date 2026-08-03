// Device-side instrumentation function, injected before every global-memory
// instruction by the NVBit backend. Port of snoopie's instrument_mem
// (sampling dropped for now; per-instruction access size added).

#include <cstdint>

#include "utils/channel.hpp"
#include "utils/utils.h"

#include "dev_common.h"

extern "C" __device__ __noinline__ void pcomm_instrument_mem(
    int pred, int opcode_id, int dev_id, uint64_t addr, uint64_t grid_launch_id,
    uint64_t pchannel_dev, int instr_index, int access_size) {
    if (!pred) return;

    int active_mask = __ballot_sync(__activemask(), 1);
    const int laneid = get_laneid();
    const int first_laneid = __ffs(active_mask) - 1;

    pcomm_gpu_access_t ma;
    for (int i = 0; i < 32; i++) {
        ma.addrs[i] = __shfl_sync(active_mask, addr, i);
    }

    ma.dev_id = dev_id;
    ma.lane_id = laneid;
    ma.opcode_id = opcode_id;
    ma.instr_index = static_cast<uint32_t>(instr_index);
    ma.access_size = static_cast<uint32_t>(access_size);

    uint64_t block_id = blockIdx.x + blockIdx.y * gridDim.x +
                        gridDim.x * gridDim.y * blockIdx.z;
    ma.thread_index = block_id * (blockDim.x * blockDim.y * blockDim.z) +
                      (threadIdx.z * (blockDim.x * blockDim.y)) +
                      (threadIdx.y * blockDim.x) + threadIdx.x;

    if (first_laneid == laneid) {
        ChannelDev* channel_dev = (ChannelDev*)pchannel_dev;
#ifndef PCOMM_INJECT_NO_PUSH
        channel_dev->push(&ma, sizeof(pcomm_gpu_access_t));
#else
        // Diagnostic build (-DPCOMM_INJECT_NO_PUSH): every instruction is still
        // instrumented — same injected calls, same register pressure, same SASS
        // perturbation — but nothing is ever pushed to the channel. Separates
        // "the injected code changes the kernel" from "push() interacts badly
        // with the workload". Never enabled in a normal build.
        (void)channel_dev;
        asm volatile("" ::"r"(ma.dev_id) : "memory");
#endif
    }
}

// Keeps the injected function from being optimized away ("BLACK MAGIC DONT
// REMOVE", per the original).
__global__ void pcomm_trickery() {
    pcomm_instrument_mem(0, 0, 0, 0, 0, 0, 0, 0);
}
