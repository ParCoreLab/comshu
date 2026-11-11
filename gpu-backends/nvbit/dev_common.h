#pragma once
// Shared between the device instrumentation function and the host drain
// thread (NVBit backend only — never crosses into libcomshu).
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
    // Which launch issued this access. NVBit hands it to the device via
    // nvbit_set_at_launch and it has always been a parameter of
    // comshu_instrument_mem; carrying it back is what lets the host bound *when*
    // the access happened. The device has no clock (reading %globaltimer per
    // instrumented instruction is on the hot path of the thing being measured),
    // so the launch time is the lower bound and the drain time is the upper one
    // — a Window, not an Instant. See
    // plans/PLAN-20260803-1021-gpumem-window-timestamps-*.md.
    //
    // Span records (lane_id -2) carry the same id in COMSHU_SPAN_LAUNCH below,
    // which predates this field. Both are set, so the two record kinds can be
    // joined on one launch without the reader caring which it is reading — that
    // is what a later tightening of the lower bound from enqueue-TSC to the
    // kernel's own %globaltimer window would key on.
    uint64_t launch_id;
} comshu_gpu_access_t;

// KERNEL SPAN RECORDS ride in the access record rather than in a struct of
// their own, and that is a deliberate constraint, not laziness: the host drain
// walks the channel at a FIXED STRIDE of sizeof(comshu_gpu_access_t), so a
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
#define COMSHU_SPAN_LANE_ID (-2)
#define COMSHU_SPAN_LAUNCH(r) ((r).addrs[0])    /* nvbit_set_at_launch id */
#define COMSHU_SPAN_TICKS(r) ((r).addrs[1])     /* %globaltimer, ns       */
#define COMSHU_SPAN_IS_END(r) ((r).opcode_id)   /* 0 = entry, 1 = exit    */
#define COMSHU_SPAN_BLOCK(r) ((r).thread_index) /* flat block index       */
