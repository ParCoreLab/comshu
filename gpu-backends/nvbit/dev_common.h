#pragma once
// Shared between the device instrumentation function and the host drain
// thread (NVBit backend only — never crosses into libpcomm).
//
// Warp-coalesced like snoopie's mem_access_t: the first active lane pushes
// one record carrying all 32 lane addresses.

#include <stdint.h>

typedef struct {
    int      dev_id;        // running device (cudaGetDevice at instrumentation)
    int      lane_id;       // -1 = flush token (end of context)
    int      opcode_id;     // index into host-side opcode table
    uint32_t instr_index;   // dense global instruction index (pc identity)
    uint32_t access_size;   // bytes per lane access (from Instr::getSize())
    uint64_t addrs[32];     // per-lane addresses (0 = inactive lane)
    uint64_t thread_index;  // flat thread index of the pushing lane
    // Which launch issued this access. NVBit hands it to the device via
    // nvbit_set_at_launch and it has always been a parameter of
    // pcomm_instrument_mem; carrying it back is what lets the host bound *when*
    // the access happened. The device has no clock (reading %globaltimer per
    // instrumented instruction is on the hot path of the thing being measured),
    // so the launch time is the lower bound and the drain time is the upper one
    // — a Window, not an Instant. See
    // plans/PLAN-20260803-1021-gpumem-window-timestamps-*.md.
    uint64_t launch_id;
} pcomm_gpu_access_t;
