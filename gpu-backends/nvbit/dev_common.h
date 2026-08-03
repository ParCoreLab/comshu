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
} pcomm_gpu_access_t;
