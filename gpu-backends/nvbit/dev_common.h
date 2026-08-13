#pragma once
// Shared between the device instrumentation function and the host drain
// thread (NVBit backend only — never crosses into libpcomm).
//
// Warp-coalesced like snoopie's mem_access_t: the first active lane pushes
// one record carrying all 32 lane addresses.

#include <stdint.h>

typedef struct {
    int      dev_id;        // running device (cudaGetDevice at instrumentation)
    int      lane_id;       // -1 = flush token, -2 = kernel span (see below)
    int      opcode_id;     // index into host-side opcode table
    uint32_t instr_index;   // dense global instruction index (pc identity)
    uint32_t access_size;   // bytes per lane access (from Instr::getSize())
    uint64_t addrs[32];     // per-lane addresses (0 = inactive lane)
    uint64_t thread_index;  // flat thread index of the pushing lane
} pcomm_gpu_access_t;

// KERNEL SPAN RECORDS ride in the access record rather than in a struct of
// their own, and that is a deliberate constraint, not laziness: the host drain
// walks the channel at a FIXED STRIDE of sizeof(pcomm_gpu_access_t), so a
// second, smaller record type would desynchronise the walk for everything
// behind it. `lane_id` is already the discriminator (-1 is the flush token), so
// -2 extends a mechanism that exists instead of changing the wire format the
// channel-deadlock work is written against.
//
// The waste is real — 288 bytes to carry 24 — and bounded: two of these per
// BLOCK per launch, against one per memory instruction per warp for the
// firehose they share the ring with.
//
// Field mapping, by name so both sides cannot drift:
#define PCOMM_SPAN_LANE_ID (-2)
#define PCOMM_SPAN_LAUNCH(r) ((r).addrs[0])    /* nvbit_set_at_launch id */
#define PCOMM_SPAN_TICKS(r) ((r).addrs[1])     /* %globaltimer, ns       */
#define PCOMM_SPAN_IS_END(r) ((r).opcode_id)   /* 0 = entry, 1 = exit    */
#define PCOMM_SPAN_BLOCK(r) ((r).thread_index) /* flat block index       */
