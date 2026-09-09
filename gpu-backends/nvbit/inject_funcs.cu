// Device-side instrumentation function, injected before every global-memory
// instruction by the NVBit backend. Port of snoopie's instrument_mem
// (sampling dropped for now; per-instruction access size added).

#include <cstdint>

#include "utils/channel.hpp"
#include "utils/utils.h"

#include "dev_common.h"

extern "C" __device__ __noinline__ void pcomm_instrument_mem(int pred, int opcode_id, int dev_id,
                                                             uint64_t addr, uint64_t grid_launch_id,
                                                             uint64_t pchannel_dev, int instr_index,
                                                             int access_size) {
    if (!pred)
        return;

    int       active_mask = __ballot_sync(__activemask(), 1);
    const int laneid = get_laneid();
    const int first_laneid = __ffs(active_mask) - 1;

    pcomm_gpu_access_t ma;
    for (int i = 0; i < 32; i++) {
        // __shfl_sync from a source lane that is not in active_mask returns an
        // undefined value, NOT the caller's addr and NOT zero. The host drain
        // contract (dev_common.h: "0 = inactive lane") is that an unused lane
        // reads back as 0, so write 0 explicitly for lanes not participating in
        // this access. active_mask and i are warp-uniform, so every active lane
        // takes the same branch each iteration and the shuffle stays collective.
        ma.addrs[i] = (active_mask & (1 << i)) ? __shfl_sync(active_mask, addr, i) : 0;
    }

    ma.dev_id = dev_id;
    ma.lane_id = laneid;
    ma.opcode_id = opcode_id;
    ma.instr_index = static_cast<uint32_t>(instr_index);
    ma.access_size = static_cast<uint32_t>(access_size);
    ma.launch_id = grid_launch_id;  // the host turns this into a time bound

    uint64_t block_id = blockIdx.x + blockIdx.y * gridDim.x + gridDim.x * gridDim.y * blockIdx.z;
    ma.thread_index = block_id * (blockDim.x * blockDim.y * blockDim.z) +
                      (threadIdx.z * (blockDim.x * blockDim.y)) + (threadIdx.y * blockDim.x) +
                      threadIdx.x;

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

// Kernel ENTRY/EXIT timestamp, for the capture where CUPTI is unavailable.
//
// This is the whole gpumem half of PLAN-20260813-0600: NVBit dlopens libcupti
// and takes the process's one CUPTI subscriber, so a gpumem capture cannot use
// the activity API and has to read the clock itself. %globaltimer is that
// clock — nanoseconds, device-wide, and measured at 131 ns per kernel for this
// pair on an H100 (1.1 % on a launch-bound microbenchmark, and nothing next to
// the module lifting NVBit is already charging).
//
// ONE THREAD PER BLOCK pushes, not one per warp and not one per grid:
//
//   per warp  would be a firehose — the thing this whole channel is careful
//             about — for a number that is warp-invariant.
//   per grid  (block 0 only) is cheaper still and WRONG: block 0 is not
//             guaranteed to start first or retire last, so its span is not the
//             kernel's. The host reduces min(entry)/max(exit) across blocks,
//             which is the kernel's actual occupancy window.
//
// %globaltimer is its own clock domain, NOT the host TSC and NOT CLOCK_REALTIME
// — measured drift -13.9 ppm (H100) / +5.7 ppm (A100), opposite signs, so it is
// anchored per GPU on the host side and resolved by pcomm-read's per-(rank,
// domain) fit like any other domain.
extern "C" __device__ __noinline__ void
pcomm_kernel_stamp(int is_end, int dev_id, uint64_t launch_id, uint64_t pchannel_dev) {
    // Thread 0 of each block, and only when it is the first active lane, so a
    // divergent entry point still pushes exactly once.
    if (threadIdx.x != 0 || threadIdx.y != 0 || threadIdx.z != 0)
        return;

    pcomm_gpu_access_t r;
    // Zeroed rather than partially filled: the access drain skips lanes whose
    // addr is 0, and a span record that leaked a non-zero addrs[] into that
    // path would be decoded as memory traffic if the -2 branch were ever
    // missed.
    for (int i = 0; i < 32; i++)
        r.addrs[i] = 0;

    uint64_t t;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));

    r.lane_id = PCOMM_SPAN_LANE_ID;
    r.dev_id = dev_id;
    r.instr_index = 0;
    r.access_size = 0;
    // Set on both records, though the drain reads it only off accesses: an
    // uninitialised field on the wire is a trap for whoever reads it next, and
    // the two kinds naming one launch identically is the join a tighter lower
    // bound would need. PCOMM_SPAN_LAUNCH predates the field; see dev_common.h.
    r.launch_id = launch_id;
    PCOMM_SPAN_LAUNCH(r) = launch_id;
    PCOMM_SPAN_TICKS(r) = t;
    PCOMM_SPAN_IS_END(r) = is_end;
    PCOMM_SPAN_BLOCK(r) = blockIdx.x + blockIdx.y * (uint64_t)gridDim.x +
                          (uint64_t)gridDim.x * gridDim.y * blockIdx.z;

    ChannelDev* channel_dev = (ChannelDev*)pchannel_dev;
#ifndef PCOMM_INJECT_NO_PUSH
    channel_dev->push(&r, sizeof(pcomm_gpu_access_t));
#else
    (void)channel_dev;
    asm volatile("" ::"r"(r.dev_id) : "memory");
#endif
}

// There was an entry function here — `pcomm_trickery`, calling both injected
// functions to keep them from being optimised away ("BLACK MAGIC DONT REMOVE",
// per the original). It is gone, and it must stay gone.
//
// An entry function in THIS translation unit is what made
// `-Xptxas -astoolspatch` impossible to apply here: ptxas refuses with "Entry
// function is not allowed when command line option '--compile-as-tools-patch'
// is specified". The build's answer was CUDA_SEPARABLE_COMPILATION plus the flag
// at $<DEVICE_LINK:...>, where -dc has already had ptxas generate the SASS. The
// flag was accepted and did nothing, and without it ptxas allocated this
// function uniform registers up to UR27. NVBit preserves only UR4:UR5 across an
// injected call, so every kernel that had a live pointer in UR6+ at an
// instrumented instruction got it destroyed — the "64-bit index" illegal
// access, which was never about the index. See ../nvbit-bug-report.
//
// The anchor was never needed: --keep-device-functions retains both functions on
// its own in a whole-program compile, which is what NVBit's own sample tools
// rely on. Re-adding any __global__ here silently reverts the fix, because the
// build would have to move the flag back to the device link to compile at all.
