// The injected device functions — one per mode, so the modes differ ONLY in
// what runs at the instrumented instruction. Compiled the way NVBit requires
// for injected code (see job.sbatch): no rdc, --keep-device-functions,
// -Xptxas -astoolspatch, a register cap.
#include <cstdint>

#include "utils/channel.hpp"

// A call that does nothing. Separates "a call is spliced in before LDL.64"
// from anything the call does.
extern "C" __device__ __noinline__ void wedge_empty(int pred) { (void)pred; }

// One global atomic per active lane. A side effect with no channel.
extern "C" __device__ __noinline__ void wedge_atomic(int pred, uint64_t counter) {
    if (pred)
        atomicAdd(reinterpret_cast<unsigned long long*>(counter), 1ull);
}

// NVBit's stock channel, exactly as its mem_trace example uses it: the first
// active lane pushes a 16-byte record. This is the shape comshu's push() has.
extern "C" __device__ __noinline__ void wedge_channel(int pred, uint64_t pchannel) {
    if (!pred)
        return;
    const int active = __ballot_sync(__activemask(), 1);
    int       lane;
    asm volatile("mov.u32 %0, %%laneid;" : "=r"(lane));
    if (lane == __ffs(active) - 1) {
        uint64_t rec[2] = {static_cast<uint64_t>(active), 0x1006ull};
        reinterpret_cast<ChannelDev*>(pchannel)->push(rec, sizeof(rec));
    }
}

// comshu's own injected body, copied: 32 warp shuffles gathering every lane's
// address, then the first active lane pushes the whole record (~300 B). This is
// the shape the original bisect ran (gpu-backends/nvbit/inject_funcs.cu), with
// the record type spelled out here so the probe still includes nothing of
// comshu. If 'channel' completes and this hangs, the trigger is the record
// size, the shuffles, or the register pressure they bring.
struct WedgeRecord {
    int      dev_id, lane_id, opcode_id;
    uint32_t instr_index, access_size;
    uint64_t addrs[32];
    uint64_t thread_index, launch_id;
};

extern "C" __device__ __noinline__ void wedge_shfl(int pred, uint64_t addr, uint64_t pchannel) {
    if (!pred)
        return;
    const int active = __ballot_sync(__activemask(), 1);
    int       lane;
    asm volatile("mov.u32 %0, %%laneid;" : "=r"(lane));
    WedgeRecord r;
    for (int i = 0; i < 32; i++)
        r.addrs[i] = (active & (1 << i)) ? __shfl_sync(active, addr, i) : 0;
    r.dev_id = 0;
    r.lane_id = lane;
    r.opcode_id = 0;
    r.instr_index = 1006;
    r.access_size = 8;
    r.thread_index = threadIdx.x;
    r.launch_id = 0;
    if (lane == __ffs(active) - 1)
        reinterpret_cast<ChannelDev*>(pchannel)->push(&r, sizeof(r));
}
