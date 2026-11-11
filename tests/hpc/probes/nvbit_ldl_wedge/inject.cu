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
