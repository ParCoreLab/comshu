// NVBit backend for the gpumem provider — port of snoopie's mem_multigpu.cu,
// reduced to the capture path and re-targeted at pcomm::gpumem::Engine:
//
//   nvbit_at_cuda_event  -> instrument mem ops, track allocations
//   instrumentation pass -> Engine::on_source_line (SourceLine artifacts)
//   alloc callbacks      -> Engine::on_alloc (MemObject entities + registry)
//   recv thread          -> pcomm_gpu_access_t -> RawAccess -> Engine::process
//
// Not yet ported from snoopie (phase-1 follow-ups): sampling, python/NCCL
// attribution, data-object callstack attribution (adm), cuMemcpyDtoD synthesis.
//
// Vendor types (CUcontext, Instr, ...) are allowed HERE and nowhere above the
// gpu-backends/ boundary.

#include <cassert>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <string>

#include <sys/stat.h>
#include <vector>

#include "nvbit.h"
#include "nvbit_tool.h"
#include <atomic>
#include <chrono>

#include "utils/channel.hpp"

#include "dev_common.h"
#include "engine.hpp"

#include "pcomm/core/ids.hpp"
#include "pcomm/core/vocab.hpp"
#include "pcomm/engine_abi.h"
// Same clock as the recorder and the ClockAnchors, so an access's window is
// comparable with ibverbs call intervals without any conversion. Safe here:
// this .cu is compiled by nvcc with a *gcc* host compiler, and these are host
// functions. Do not move the call into providers/gpumem/gpumem_provider.cpp —
// that TU is compiled by nvc++, which emits an undefined reference to
// __builtin_ia32_rdtsc instead of inlining it (see things-to-circle-back-to.md).
#include "pcomm/helpers/clock.hpp"

// Defined HERE rather than in inject_funcs.cu, even though it is a sibling of
// the injected code: launching a __global__ across a translation unit needs
// relocatable device code and gets it, but inject_funcs.cu is compiled with
// -maxrregcount=24 --keep-device-functions for the injection path, and a tool
// kernel has no business inheriting those. It is two instructions; it lives
// where it is launched.
__global__ void pcomm_read_globaltimer(uint64_t* out) {
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        uint64_t t;
        asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
        *out = t;
    }
}

using pcomm::attr::RawAccess;
using pcomm::core::MemOp;

// Ring size per CUDA context, host-pinned. Overridable with PCOMM_CHANNEL_SIZE
// (bytes) — needed to test whether the case 07 hang tracks the ring size, and
// useful anyway: 1 GiB *per context* is a lot to pin on a 4-GPU node, and the
// recv thread mallocs the same amount again for its staging buffer.
#define CHANNEL_SIZE_DEFAULT (1l << 30)
static long channel_size() {
    static long sz = [] {
        if (const char* e = std::getenv("PCOMM_CHANNEL_SIZE")) {
            long v = std::atol(e);
            if (v >= (1l << 20))
                return v;  // sanity floor: 1 MiB
        }
        return (long)CHANNEL_SIZE_DEFAULT;
    }();
    return sz;
}

namespace {

struct CtxState {
    ChannelDev*                  channel_dev{nullptr};
    ChannelHost                  channel_host;
    volatile unsigned long long* dbg_host{nullptr};  // PCOMM_CHANNEL_DEBUG counters
    // The recv thread's only other way out. It normally exits on the flush
    // token the terminating kernel writes; if that kernel cannot run — a
    // poisoned context, where every launch returns the sticky error — the token
    // never arrives and the thread spins while nvbit_at_ctx_term blocks in
    // pthread_join. Measured: job 44458142 hung until the walltime.
    std::atomic<bool> recv_abort{false};
};

// NVBit drives context teardown from an atexit handler, which can run after
// this .so's C++ static destructors. Every global these callbacks touch is
// therefore heap-allocated and deliberately leaked (or trivially
// destructible) so no exit-ordering can leave a callback with a dead object.
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;  // serializes callbacks

// A region some library mapped with cuMemMap, as observed — base and size, no
// interpretation. NVSHMEM's symmetric heap arrives this way; so do other
// things, and telling them apart is nvshmem_ptr's job, not ours (see
// refresh_mapped_regions).
struct MappedRegion {
    uint64_t base;
    uint64_t size;
    // Set only when this mapping's allocation was shared as a FABRIC handle:
    // the handle's 64 bytes in hex, which both sides of the share mint
    // identically and can therefore be joined on. Empty for everything else — a
    // POSIX fd names the driver node rather than the allocation (measured, MN5
    // job 44705498: all 48 exports and imports fstat'd to dev=6 ino=1729), so
    // there is nothing worth carrying.
    std::string alloc_hex;
    uint64_t    alloc_offset;  // cuMemMap's own offset into the allocation
    bool        exporter;      // we created and exported it, vs imported it
};

struct Globals {
    // CUmemGenericAllocationHandle -> (fabric hex, did we export it). Written
    // by the export/import hooks and read at cuMemMap, which is the only place
    // the local address of the mapping is known.
    std::unordered_map<unsigned long long, std::pair<std::string, bool>> fabric_of;
    std::unordered_map<CUcontext, CtxState*>                             ctx_state;
    std::unordered_set<CUfunction>                                       instrumented;
    std::map<std::string, int>                                           opcode_to_id;
    std::vector<std::string>                                             id_to_opcode;
    std::vector<MemOp>                                                   id_to_memop;
    std::string                                                          kernel_filter = "all";
    // Touched only by the CUDA-callback thread, under g_mutex. The recv thread
    // never reads these — it used to, and needed an atomically published
    // snapshot to do it safely, because sorting the live vector while the
    // callback thread could be push_back()ing into it is a use-after-free.
    // Resolving at launch time instead makes the sharing go away entirely.
    std::vector<MappedRegion>    mapped_regions;
    std::unordered_set<uint64_t> mapped_bases_seen;
    size_t                       regions_resolved = 0;  // prefix already attributed from CUDA
    size_t                       heap_resolved = 0;     // prefix already offered to NVSHMEM
};
Globals& G() {
    static Globals* g = new Globals();
    return *g;
}

pcomm::gpumem::Engine* g_engine = nullptr;
bool                   g_skip_callback = false;
bool                   g_shutdown_done = false;
uint32_t               g_next_instr_index = 0;
// STARTS AT 1, NOT 0, AND THAT IS THE WHOLE REASON IT IS WRITTEN OUT HERE.
//
// This id becomes GpuAccess.launch and GpuKernel.correlation, and in BOTH of
// those 0 is documented as the sentinel for "unknown" (core/records.hpp). Minted
// from 0, the first launch of every process got a real id that is
// indistinguishable from the sentinel — so the reader's join, which excludes
// correlation = 0 by design, could never match it, and every access that kernel
// issued fell back to the wide host-TSC window with nothing to say why.
//
// Silent, permanent, and once per process. Measured on du04 (CUDA 13.0, driver
// 580, 4x A100) with tests/hpc/cases/03_p2p_kernel: correlations came out 0, 4,
// 8, 12 and exactly 4096 of 16384 gpu_access rows — 25.0% — could not join. The
// share is 1/N for an N-kernel run, so it shrinks with runtime and never reaches
// zero.
//
// The ring below is unaffected: it is indexed by id % kLaunchRing and its owner
// array is filled with ~0ull, so it never confused an unwritten slot for id 0.
uint64_t               g_grid_launch_id = 1;

// --- Kernel lifetimes, the gpumem half of PLAN-20260813-0600 ---------------
//
// Read once at init from the same variable the CUPTI backend consults, so the
// two sources are configured by one switch and can never both be on: with NVBit
// in the process, CUPTI cannot register at all, and the launcher clears the
// variable for that combination anyway.
bool g_kernel_timing = false;

// What only the HOST knows about a launch: which function, and its geometry.
// The device supplies only (launch_id, block, timestamp) — passing a name
// through the channel would mean a string per launch on the hot path.
struct LaunchMeta {
    std::string name;
    uint32_t    grid[3]{};
    uint16_t    block[3]{};
    int         dev_id{-1};
};

// launch_id -> what the device reported, reduced as it arrives.
//
// min(entry) / max(exit) ACROSS BLOCKS is the kernel's real occupancy window.
// Taking block 0's pair instead would be cheaper and wrong — nothing orders
// block 0 first or last — and the difference is exactly the tail this view is
// for.
struct LaunchSpan {
    uint64_t t0 = UINT64_MAX;
    uint64_t t1 = 0;
    int      dev_id = -1;
    uint32_t blocks_seen = 0;
};

pthread_mutex_t                          g_span_mutex = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<uint64_t, LaunchMeta> g_launch_meta;  // written by the callback thread
std::unordered_map<uint64_t, LaunchSpan> g_launch_span;  // written by the recv thread

// (globaltimer, CLOCK_REALTIME) per device, sampled at first launch and at
// teardown. Two points per GPU is what pcomm-read's per-(rank, domain) fit
// needs, and per-GPU is not optional: drift measured at -13.9 ppm on H100 and
// +5.7 ppm on A100, opposite signs.
std::unordered_set<int> g_gpu_anchored;

// --- launch times: the lower bound of every access's window ------------------
//
// The device has no clock we share, so an access is bounded rather than dated:
// it cannot have happened before its kernel was launched (here), and it must
// have happened before the drain thread dequeued it (the recv loop). See
// plans/PLAN-20260803-1021-gpumem-window-timestamps-*.md.
//
// A ring, not a map: ids come from g_grid_launch_id++ so they are dense and
// monotonic, and a long run issues millions of them — an unbounded map would be
// a leak that grows with runtime. Old entries age out and their accesses fall
// back to an open lower bound, which is honest; inventing one would not be.
//
// NB this is a SEPARATE structure from g_launch_span/g_launch_meta above, which
// the kernel-span work keys by the same id. They are not merged because they are
// written and read by different threads under different locks and with different
// lifetimes: this one is host TSC, written on the CUDA-callback thread under
// g_mutex and read on the recv thread; those are globaltimer, reduced on the
// recv thread under g_span_mutex and drained at teardown. One id, two clocks —
// which is exactly the pairing a later tightening of ticks_lo would exploit.
constexpr std::size_t kLaunchRing = 1u << 16;    // 64Ki launches, 512 KB
std::uint64_t*        g_launch_ticks = nullptr;  // indexed by launch_id % kLaunchRing
std::uint64_t*        g_launch_owner = nullptr;  // which id that slot holds
unsigned long long    g_launch_unknown = 0;      // accesses with no launch time

// Records the launch time for `id`. Called under g_mutex from the CUDA callback.
inline void note_launch_time(std::uint64_t id) {
    if (!g_launch_ticks) {
        g_launch_ticks = new std::uint64_t[kLaunchRing]();
        g_launch_owner = new std::uint64_t[kLaunchRing];
        std::fill(g_launch_owner, g_launch_owner + kLaunchRing, ~0ull);
    }
    const std::size_t slot = id % kLaunchRing;
    g_launch_owner[slot] = id;
    g_launch_ticks[slot] = pcomm::helpers::TscClock::now();
}

// The launch time for `id`, or 0 if that launch has aged out of the ring or was
// never seen (e.g. a CUDA-graph node launch — nvbit_at_graph_node_launch is
// still a stub). 0 means "unknown", and the pipeline leaves the window open
// below rather than fabricating a bound.
inline std::uint64_t launch_time(std::uint64_t id) {
    if (!g_launch_ticks)
        return 0;
    const std::size_t slot = id % kLaunchRing;
    return g_launch_owner[slot] == id ? g_launch_ticks[slot] : 0;
}

struct MutexLock {
    explicit MutexLock(pthread_mutex_t& m) : m_m(&m) { pthread_mutex_lock(m_m); }
    ~MutexLock() { pthread_mutex_unlock(m_m); }
    pthread_mutex_t* m_m;
};

// --- Ownership of cuMemMap'd memory, NVSHMEM's symmetric heap included -------
//
// This used to be derived from NVSHMEM's *private* VA layout: sort the cuMemMap
// regions, and region i belongs to PE (mype + i) % npes with npes taken from
// how many regions had been seen. Every part of that was wrong. NVSHMEM maps
// ~76 regions for 8 PEs, so npes was wrong; the index was positional, so the
// answer depended on how many regions existed at the moment a record happened
// to be drained; and ownership is not obliged to be contiguous or rotational.
// Measured mem_dev = 64,65,66,67 where the truth is 1,2,3 (job 44111484 vs
// 44113361), which RemoteFilter (run_device != mem_device) then turned into
// phantom remote traffic — case 07 doubled, 196608 -> 393216 accesses per rank.
// A 2.x sibling path hardcoded a base VA and a stride outright.
//
// So we ask NVSHMEM instead. nvshmem_ptr(a, pe) returns the local address of PE
// pe's copy of symmetric object a, or NULL when pe is not P2P-reachable — which
// is precisely the map the old code reconstructed. Resolved with dlsym at
// runtime, so the provider keeps no build or link dependency on NVSHMEM: no
// symbols means the application is not using it and the path is skipped.
//
// Validated on MN5 (NVSHMEM 3.2.5, jobs 44135828/44136107) *before* being built
// on, in four configurations — 4 PEs on one node, 8 PEs over two nodes with
// IBRC and with IBGDA, and NVSHMEM_DISABLE_P2P:
//   - nvshmem_ptr(a, my_pe) == a for every local symmetric address probed:
//     allocation bases, interiors, and a 64 MiB object's last byte;
//   - NULL for everything that is *not* local symmetric memory — cudaMalloc
//     pointers, host addresses, junk VAs, and peers' mappings of the heap — so
//     "non-NULL for my_pe" is a sound test for "this region is my heap", with
//     no false positives to mis-expand;
//   - non-NULL exactly for the P2P-reachable PEs: 0-3 within a node, NULL for
//     the four PEs across the IB link (which is why those ranks legitimately
//     record no NVLink traffic), and self only under NVSHMEM_DISABLE_P2P;
//   - callable from a thread other than the one that initialised NVSHMEM, which
//     the CUDA-callback thread this runs on may well be.
struct NvshmemApi {
    void* (*ptr)(const void*, int){nullptr};
    int (*my_pe)(){nullptr};
    int (*n_pes)(){nullptr};
    int (*init_status)(){nullptr};
    bool ok() const { return ptr && my_pe && n_pes && init_status; }
};

// From nvshmem_constants.h; not included, since the whole point is to have no
// build dependency on NVSHMEM.
constexpr int kNvshmemStatusIsInitialized = 2;

// Lazy on purpose: NVSHMEM is not loaded when the tool's nvbit_at_init runs, so
// resolving there would fail permanently. Called only under g_mutex.
const NvshmemApi& nvshmem_api() {
    static NvshmemApi* api = new NvshmemApi();  // leaked: exit-order safe
    if (!api->ok()) {
        api->ptr = (void* (*)(const void*, int))dlsym(RTLD_DEFAULT, "nvshmem_ptr");
        api->my_pe = (int (*)())dlsym(RTLD_DEFAULT, "nvshmem_my_pe");
        api->n_pes = (int (*)())dlsym(RTLD_DEFAULT, "nvshmem_n_pes");
        api->init_status = (int (*)())dlsym(RTLD_DEFAULT, "nvshmemx_init_status");
    }
    return *api;
}

// A CUmemFabricHandle's 64 bytes as hex. Hex rather than raw because the intern
// table's string domain is dumped into the capture and read back as text; 128
// printable characters cost nothing at this frequency — one per shared
// allocation, not per access.
inline std::string fabric_hex(const void* blob) {
    static const char* kHex = "0123456789abcdef";
    const auto*        b = static_cast<const unsigned char*>(blob);
    std::string        s(128, '0');
    for (int i = 0; i < 64; i++) {
        s[2 * i] = kHex[b[i] >> 4];
        s[2 * i + 1] = kHex[b[i] & 0xF];
    }
    return s;
}

// PROBE, gated on PCOMM_TRACE_VMM=1 — which memory-sharing doors does this
// workload actually use, and with what identity?
//
// It exists because the last attempt to name VMM memory was built on an
// assumption about these doors and cost three MN5 jobs to disprove (see
// plans/PLAN-20260817-0705). The question this answers first:
//
//   - does anything export/import a shareable handle, or is peer sharing done
//     with the legacy cuIpc* pair, or neither;
//   - what handle TYPE, since a POSIX fd is process-local and a fabric handle is
//     not, and the join key differs completely between them;
//   - for an fd, whether (st_dev, st_ino) is stable across processes — that is
//     the only thing that could make an fd-based export joinable at all.
//
// Diagnostic only: no record, no registry entry, and silent unless asked.
inline bool vmm_trace_on() {
    static const bool on = [] {
        const char* e = std::getenv("PCOMM_TRACE_VMM");
        return e && *e && std::strcmp(e, "0") != 0;
    }();
    return on;
}

// An OS handle's global identity, when it is a file descriptor. Two processes
// that hold the same underlying kernel object see the same (st_dev, st_ino)
// even though the fd NUMBERS differ — which is exactly the property a join key
// needs and the fd itself lacks.
inline void vmm_trace_fd(const char* what, long long fd, unsigned int type) {
    struct stat st{};
    if (fd >= 0 && fstat((int)fd, &st) == 0)
        std::fprintf(stderr, "[pcomm-vmm] %s type=%u fd=%lld dev=%llu ino=%llu\n", what, type, fd,
                     (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
    else
        std::fprintf(stderr, "[pcomm-vmm] %s type=%u handle=%lld (not an fd)\n", what, type, fd);
    std::fflush(stderr);
}

// Which GPU does the memory at `p` live on? CUDA's own answer, in the same
// space run_device is in. Negative when it cannot say.
int device_of(uint64_t p, int ndev, const char* what) {
    int dev = -1;
    if (cuPointerGetAttribute(&dev, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL, (CUdeviceptr)p) !=
        CUDA_SUCCESS)
        return -1;
    // The guard the old scheme could not have had: an owner must name a GPU
    // that exists. mem_dev = 64 on a 4-GPU node would have stopped here,
    // loudly, instead of quietly inflating the capture.
    if (dev < 0 || dev >= ndev) {
        std::fprintf(stderr,
                     "[pcomm-gpumem] %s owner out of range: dev=%d of %d "
                     "GPUs; 0x%llx dropped\n",
                     what, dev, ndev, (unsigned long long)p);
        return -1;
    }
    return dev;
}

// Turn newly observed cuMemMap regions into (range -> device) entries in the
// alloc registry the drain path already consults. Nothing vendor-shaped
// survives on the hot path: attribution is one range lookup, as it is for
// cudaMalloc'd memory.
//
// Called at kernel launch, from the CUDA-callback thread, holding g_mutex.
// That is the one moment with both properties this needs: NVSHMEM is
// initialised (so nvshmem_ptr is safe to call), and every region the kernel can
// touch is already mapped — you cannot launch a kernel against memory that is
// not. Resolving from the recv thread instead does not work, and the way it
// fails is instructive: the ring never fills at the default 1 GiB, so the
// *entire* capture arrives in the single flush at context teardown, in the same
// buffer as the terminate token. By then the application has finalized NVSHMEM,
// so the one batch that needs the map is the one batch that cannot build it
// (job 44137162: no crash, and every access filtered out).
//
// CUDA calls are safe here despite g_mutex: NVBit does not deliver callbacks
// for calls made from inside a callback, which is what handle_alloc's
// cudaGetDevice has always relied on.
void refresh_mapped_regions() {
    auto&        g = G();
    const size_t n = g.mapped_regions.size();
    if (!g_engine || (g.regions_resolved == n && g.heap_resolved == n))
        return;

    int ndev = 0;
    cudaGetDeviceCount(&ndev);

    // 1. Every mapped region, from CUDA. cuMemMap is not NVSHMEM's alone —
    //    NCCL registers its peer buffers through the same APIs, and the
    //    cuMemAlloc-fed registry never sees those, so without this they resolve
    //    to run_device and RemoteFilter drops real peer traffic. Case 05's
    //    peer traffic used to appear only because the NVSHMEM positional guess
    //    was being applied to NCCL's regions — it reported mem_dev up to 8 on a
    //    4-GPU node, i.e. it was fabricating the answer that made the case pass.
    const bool verbose = std::getenv("PCOMM_VERBOSE") != nullptr;
    int        by_cuda = 0, fabric = 0;
    for (size_t i = g.regions_resolved; i < n; i++) {
        const auto& r = g.mapped_regions[i];
        const int   dev = device_of(r.base, ndev, "mapped region");
        if (dev >= 0) {
            g_engine->allocs().add_range(r.base, r.size, static_cast<pcomm::core::DeviceId>(dev));
            by_cuda++;
            // A fabric-shared allocation is the one case where a peer mapping
            // can be tied back to the owner's address, so record the pairing.
            // Emitted HERE rather than at cuMemMap because this is where the
            // device is known, and device_of is the answer the rest of this
            // function already trusts over anything inferred.
            if (!r.alloc_hex.empty()) {
                g_engine->on_peer_mapping(r.alloc_hex, r.base, r.size, r.alloc_offset,
                                          static_cast<pcomm::core::DeviceId>(dev), r.exporter);
                fabric++;
            }
        }
    }
    if (verbose && g.regions_resolved != n)
        std::fprintf(stderr,
                     "[pcomm-gpumem] %d/%zu new mapped regions attributed by CUDA, "
                     "%d fabric-shared (%zu total)\n",
                     by_cuda, n - g.regions_resolved, fabric, n);
    g.regions_resolved = n;

    // 2. NVSHMEM's symmetric heap, authoritatively. Step 1 already covers every
    //    peer mapping we *observed* a cuMemMap for; this covers the rest and is
    //    the only source that knows a region is symmetric memory at all rather
    //    than inferring it from where it happens to sit in the VA space.
    const NvshmemApi& api = nvshmem_api();
    if (!api.ok()) {
        // Not an error: most workloads are not NVSHMEM. Say it once under
        // PCOMM_VERBOSE rather than either staying silent or annotating every
        // run that was never going to have a symmetric heap.
        static bool said = false;
        if (!said && std::getenv("PCOMM_VERBOSE")) {
            said = true;
            std::fprintf(stderr, "[pcomm-gpumem] no NVSHMEM symbols; mapped regions "
                                 "attributed from CUDA alone\n");
        }
        return;  // retried at the next launch — NVSHMEM may load later
    }
    // nvshmem_ptr dereferences NVSHMEM's heap state, which does not exist before
    // nvshmem_init or after nvshmem_finalize; calling it outside that window
    // segfaults on a null+0x20 (all four ranks of case 04, job 44136544).
    // nvshmem_my_pe and nvshmem_n_pes keep answering plausibly across the same
    // boundary, so they cannot be the test. nvshmemx_init_status can, and is
    // required rather than optional: without it there is no safe window to
    // check and the heap path must stay off.
    //
    // Returning here without advancing heap_resolved is the point of tracking
    // it separately from regions_resolved: a launch before nvshmem_init must
    // not mark the regions as having had their chance.
    if (api.init_status() < kNvshmemStatusIsInitialized)
        return;

    const int npes = api.n_pes();
    const int mype = api.my_pe();
    if (npes <= 0 || mype < 0 || mype >= npes)
        return;

    int mine = 0, peers = 0;
    for (size_t i = g.heap_resolved; i < n; i++) {
        const auto& r = g.mapped_regions[i];
        // Is this region *my* symmetric heap? Only then do we know what its
        // peers' copies are. Unlike the positional scheme, the answer for a
        // region never depends on how many others have been seen, so processing
        // them as they arrive is safe.
        if (!api.ptr((const void*)r.base, mype))
            continue;
        mine++;

        // This region is confirmed to be OUR symmetric heap, and that is the
        // only point at which we know it. Record it as a MemObject now: the
        // heap is VMM-mapped so on_alloc never saw it, and the mapped-region
        // pass above filed it with no object — leaving every peer access with
        // nothing to name. The handle is then attached to each peer's copy
        // below, which is sound because the heap is symmetric.
        const int  my_dev = device_of(r.base, ndev, "NVSHMEM heap (local)");
        const auto object =
            my_dev >= 0 ? g_engine->on_symmetric_region(r.base, r.size,
                                                        static_cast<pcomm::core::DeviceId>(my_dev))
                        : pcomm::core::kNullHandle;

        for (int pe = 0; pe < npes; pe++) {
            void* q = api.ptr((const void*)r.base, pe);
            if (!q)
                continue;  // not P2P-reachable: that PE's traffic crosses
                           // the IB link and belongs to the ib provider.
            // PE is not a device. The old code cast one straight to the other,
            // which is only accidentally right when PEs and GPUs are numbered
            // alike on one node. CUDA knows where the memory nvshmem_ptr just
            // named actually lives (measured, job 44136107), so ask it rather
            // than inherit the conflation.
            const int dev = device_of((uint64_t)q, ndev, "NVSHMEM heap");
            if (dev >= 0) {
                // r.base is the SYMMETRIC address of this region and q is where
                // PE `pe`'s copy of it is mapped locally. Passing both is what
                // lets an access through q name the object: the heap is
                // symmetric, so the buffer this process recorded at r.base is
                // the same logical buffer, at the same offset. Dropping r.base
                // here is why gpu_access.object was 0 for every recorded access.
                g_engine->on_peer_range((uint64_t)q, r.size,
                                        static_cast<pcomm::core::DeviceId>(dev), object, r.base);
                peers++;
            }
        }
    }
    if (verbose && g.heap_resolved != n)
        std::fprintf(stderr,
                     "[pcomm-gpumem] NVSHMEM pe %d/%d: %d of %zu new regions "
                     "are my heap, %d peer ranges registered\n",
                     mype, npes, mine, n - g.heap_resolved, peers);
    g.heap_resolved = n;
}

MemOp opcode_to_memop(const std::string& opcode) {
    if (opcode.rfind("LD", 0) == 0)
        return MemOp::Load;
    if (opcode.rfind("ST", 0) == 0)
        return MemOp::Store;
    if (opcode.rfind("ATOM", 0) == 0 || opcode.rfind("RED", 0) == 0)
        return MemOp::Atomic;
    return MemOp::Load;
}

void instrument_function_if_needed(CUcontext ctx, CUfunction func) {
    std::vector<CUfunction> related = nvbit_get_related_functions(ctx, func);
    related.push_back(func);

    for (auto f : related) {
        if (!G().instrumented.insert(f).second)
            continue;

        int dev_id = -1;
        cudaGetDevice(&dev_id);

        const std::vector<Instr*>& instrs = nvbit_get_instrs(ctx, f);

        // Kernel entry/exit timestamps — its OWN loop, before the memory one,
        // and that separation is required rather than tidy. The loop below
        // `continue`s on any instruction that is not a global memory reference,
        // so an EXIT would never reach an injection placed inside it; it also
        // applies the PCOMM_INSTR_LO/HI bisect window, which must not silently
        // disable kernel timing while someone is bisecting an unrelated hang.
        //
        // Two injection sites per function: the first instruction, and every
        // EXIT (a function can have several). This is the gpumem half of
        // PLAN-20260813-0600 — NVBit holds the process's only CUPTI subscriber,
        // so with gpumem loaded the clock has to be read in-kernel instead.
        // Measured at 131 ns/kernel on an H100, against module lifting that
        // already costs ~435 s.
        if (g_kernel_timing && !instrs.empty()) {
            for (std::size_t i = 0; i < instrs.size(); i++) {
                Instr*     instr = instrs[i];
                const bool is_exit = std::strncmp(instr->getOpcode(), "EXIT", 4) == 0;
                if (i != 0 && !is_exit)
                    continue;
                nvbit_insert_call(instr, "pcomm_kernel_stamp", IPOINT_BEFORE);
                nvbit_add_call_arg_const_val32(instr, is_exit ? 1 : 0);
                nvbit_add_call_arg_const_val32(instr, dev_id);
                nvbit_add_call_arg_launch_val64(instr, 0);  // grid launch id
                nvbit_add_call_arg_const_val64(instr, (uint64_t)G().ctx_state[ctx]->channel_dev);
            }
        }

        for (auto instr : instrs) {
            // Only GLOBAL (and GENERIC, which resolves to global at run time)
            // can be communication. LOCAL is per-thread stack and spilled
            // registers; SHARED is intra-block. Instrumenting them produced
            // records that are meaningless for a communication trace, and it
            // is the majority of injected calls: in one 64-instruction window
            // of nvshmemi_transfer_rma_p<int>, 84 of the accesses were
            // LDS/LDL/STL.
            //
            // It is also the case 07 hang. Bisected to a single instruction
            // (job 43920181): instrumenting index 1006, an LDL.64 at
            // +0x19680 inside nvshmemi_transfer_rma_p<int>, flips the run from
            // completing in 22s to hanging. Instrumenting everything below it,
            // including the 64-bit global atomic and the STRONG.SYS stores
            // around it, completes.
            {
                const auto sp = instr->getMemorySpace();
                if (sp != InstrType::MemorySpace::GLOBAL && sp != InstrType::MemorySpace::GENERIC &&
                    sp != InstrType::MemorySpace::GLOBAL_TO_SHARED) {
                    continue;
                }
            }

            auto [it, inserted] =
                G().opcode_to_id.emplace(instr->getOpcode(), (int)G().id_to_opcode.size());
            if (inserted) {
                G().id_to_opcode.emplace_back(instr->getOpcode());
                G().id_to_memop.push_back(opcode_to_memop(instr->getOpcode()));
            }
            int opcode_id = it->second;

            uint32_t instr_index = g_next_instr_index++;

            // PCOMM_INSTR_LO/HI: instrument only instructions whose dense index
            // falls in [LO, HI). Diagnostic for case 07 — the hang needs push()
            // to run, so bisecting the instrumented set names the instruction
            // whose records trigger it. Indices are assigned in a fixed order
            // (module load order, then instruction offset), so a given index
            // denotes the same instruction across runs of the same binary.
            // Unset = instrument everything, the normal path.
            {
                static const uint32_t lo = [] {
                    const char* e = std::getenv("PCOMM_INSTR_LO");
                    return e ? (uint32_t)std::strtoul(e, nullptr, 10) : 0u;
                }();
                static const uint32_t hi = [] {
                    const char* e = std::getenv("PCOMM_INSTR_HI");
                    return e ? (uint32_t)std::strtoul(e, nullptr, 10) : UINT32_MAX;
                }();
                if (instr_index < lo || instr_index >= hi)
                    continue;
            }

            // PCOMM_INSTR_DUMP: print the index -> instruction mapping so a
            // bisect result (an index) can be turned back into an opcode, a
            // function and an offset. Diagnostic only.
            if (std::getenv("PCOMM_INSTR_DUMP")) {
                const char* fn = nvbit_get_func_name(ctx, f, true);
                std::fprintf(stderr,
                             "[pcomm-instr] idx=%u opcode=%s space=%d size=%d off=0x%x fn=%s\n",
                             instr_index, instr->getOpcode(), (int)instr->getMemorySpace(),
                             instr->getSize(), (unsigned)instr->getOffset(), fn ? fn : "?");
            }

            // Source-line artifact (nvbit_get_line_info needs -lineinfo in the
            // target app; silently absent otherwise, like snoopie).
            char*    file_name = nullptr;
            char*    dir_name = nullptr;
            uint32_t line = 0;
            if (nvbit_get_line_info(ctx, f, instr->getOffset(), &file_name, &dir_name, &line) &&
                line != 0) {
                g_engine->on_source_line(instr_index, file_name ? file_name : "",
                                         dir_name ? dir_name : "", line);
            }

            int mref_idx = 0;
            for (int i = 0; i < instr->getNumOperands(); i++) {
                const InstrType::operand_t* op = instr->getOperand(i);
                if (op->type != InstrType::OperandType::MREF)
                    continue;

                nvbit_insert_call(instr, "pcomm_instrument_mem", IPOINT_BEFORE);
                nvbit_add_call_arg_guard_pred_val(instr);
                nvbit_add_call_arg_const_val32(instr, opcode_id);
                nvbit_add_call_arg_const_val32(instr, dev_id);
                nvbit_add_call_arg_mref_addr64(instr, mref_idx);
                nvbit_add_call_arg_launch_val64(instr, 0);  // grid launch id
                nvbit_add_call_arg_const_val64(instr, (uint64_t)G().ctx_state[ctx]->channel_dev);
                nvbit_add_call_arg_const_val32(instr, (int)instr_index);
                nvbit_add_call_arg_const_val32(instr, (int)instr->getSize());
                mref_idx++;
            }
        }
    }
}

// Launch pcomm_read_globaltimer and pair its result with CLOCK_REALTIME.
//
// The host clock is read on BOTH sides of the device read and the midpoint is
// used, because the pairing is only as tight as that bracket — measured at
// 30-170 us on an idle H100. That is wide next to an 11 us kernel, which is why
// two anchors far apart are what matter: the fit recovers the RATE from their
// separation, and the bracket error stays a fixed offset rather than a skew.
//
// Called from the CUDA-callback thread with g_mutex held and g_skip_callback
// set, so our own launch does not recurse into nvbit_at_cuda_event — the same
// contract handle_alloc's cudaGetDevice already relies on.
// BEST OF N, not one shot, and the buffer is allocated once. Both matter, and
// the first version of this had neither:
//
// The pairing is only as good as the host bracket around the device read, and
// under NVBit that bracket is wide and erratic — the first implementation
// cudaMalloc'd inside it and took a single sample, and the resulting fit
// reported +111.7 ppm of "drift" on a GPU the clean probe measured at -13.9.
// That was not the clock moving, it was the anchor being read badly, and
// because a two-point fit turns offset error at either end into RATE error, it
// showed up as skew over the whole capture rather than as a constant shift.
//
// Keeping the tightest of N brackets is the cheap fix: the noise is one-sided
// (a bracket can only be too wide, never too narrow), so the minimum is the
// best estimator available without a proper round-trip protocol.
// ONE BUFFER PER DEVICE, AND THE DEVICE MADE CURRENT AROUND THE LAUNCH. Both
// halves are load-bearing and neither was here, because until 2026-08-17 this
// function had never run in a process with more than one GPU in it.
//
// It was `static uint64_t* d_out` — one allocation for the whole process, made
// in whichever context happened to be current at the first call. A device
// pointer belongs to the device it was allocated on, so every subsequent
// `pcomm_read_globaltimer<<<1,1>>>(d_out)` on a *different* device wrote through
// a foreign pointer: an illegal memory access, which poisons the context and
// costs the whole capture. Case 03 is the only case in the suite that puts four
// GPUs in one process (cudaSetDevice in a loop, four contexts), and it is the
// only one that failed — job 44713195, `p2p_kernel.cu:64: an illegal memory
// access was encountered` on the cudaDeviceSynchronize that surfaces it, with
// exactly one `gpu 0 anchor` line printed before the other three faulted.
//
// The second half matters at ctx term, which loops `for (int dev :
// g_gpu_anchored) sample_gpu_anchor(dev)` without switching device. A kernel
// launches on the CURRENT device regardless of what dev_id says, so without the
// switch the closing anchor samples the wrong GPU's clock and files it under the
// right GPU's domain — a wrong anchor rather than a missing one, which is worse:
// pcomm-read fits rate from the two anchors, so it would have silently skewed
// every span in that domain.
// A PLAIN ARRAY, NOT A MAP, AND THAT IS NOT A MICRO-OPTIMISATION. The first
// version of this fix used `static std::unordered_map<int, uint64_t*>` and
// segfaulted every gpumem case on job 44713978 — at address 0x8, inside
// nvbit_at_ctx_term, reached from an exit handler (on_exit ->
// __libc_start_main). A function-local static with a non-trivial destructor is
// destroyed during exit, and ctx term also runs during exit, in an order
// nothing here controls: touching the map after its destructor ran is a
// use-after-free. nvbit_at_term below already states the rule — "statics may be
// gone" — and this is the same hazard one frame earlier.
//
// So the state a teardown path touches must be trivially destructible. An array
// indexed by device ordinal is, and 32 is generous: NVML tops out well below it
// and an out-of-range ordinal simply gets no anchor rather than corrupting
// memory.
constexpr int kMaxAnchoredDevices = 32;
uint64_t*     g_anchor_buf[kMaxAnchoredDevices] = {};  // per device; leaked by design

void sample_gpu_anchor(int dev_id) {
    if (dev_id < 0 || dev_id >= kMaxAnchoredDevices)
        return;

    int prev = -1;
    if (cudaGetDevice(&prev) != cudaSuccess)
        return;
    if (prev != dev_id && cudaSetDevice(dev_id) != cudaSuccess)
        return;
    // Restore on every exit path below, of which there are several.
    struct DeviceGuard {
        int prev, cur;
        ~DeviceGuard() {
            if (prev != cur)
                cudaSetDevice(prev);
        }
    } guard{prev, dev_id};

    if (!g_anchor_buf[dev_id] &&
        cudaMalloc(&g_anchor_buf[dev_id], sizeof(uint64_t)) != cudaSuccess) {
        g_anchor_buf[dev_id] = nullptr;  // allocated on dev_id: it is current here
        return;
    }
    uint64_t* const d_out = g_anchor_buf[dev_id];

    const auto ns = [](const timespec& t) {
        return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
    };

    uint64_t best_window = UINT64_MAX, best_ticks = 0, best_wall = 0;
    cudaDeviceSynchronize();  // once, outside the measured region
    for (int i = 0; i < 8; i++) {
        timespec a{}, b{};
        clock_gettime(CLOCK_REALTIME, &a);
        pcomm_read_globaltimer<<<1, 1>>>(d_out);
        if (cudaDeviceSynchronize() != cudaSuccess)
            return;
        clock_gettime(CLOCK_REALTIME, &b);
        uint64_t ticks = 0;
        if (cudaMemcpy(&ticks, d_out, sizeof(ticks), cudaMemcpyDeviceToHost) != cudaSuccess ||
            ticks == 0)
            continue;
        const uint64_t window = ns(b) - ns(a);
        if (window < best_window) {
            best_window = window;
            best_ticks = ticks;
            best_wall = ns(a) + window / 2;
        }
    }
    if (!best_ticks)
        return;
    if (std::getenv("PCOMM_VERBOSE"))
        std::fprintf(stderr, "[pcomm-nvbit] gpu %d anchor: window=%lluns\n", dev_id,
                     (unsigned long long)best_window);
    pcomm_engine_clock_anchor((uint16_t)(pcomm::core::kDomainGpuTimerBase + dev_id), best_ticks,
                              best_wall);
}

// Grid/block dimensions, as the launch API reported them. Carried in a struct
// rather than six parameters so the three launch entry points below cannot get
// the order wrong between them.
struct LaunchGeom {
    unsigned gx = 0, gy = 0, gz = 0, bx = 0, by = 0, bz = 0;
};

void handle_launch(CUcontext ctx, CUfunction func, LaunchGeom geom = {}) {
    // Attribute any regions mapped since the last launch, here rather than on
    // the drain path — see refresh_mapped_regions for why this is the only
    // moment that works. Cheap: an integer compare unless a region appeared.
    refresh_mapped_regions();

    std::vector<CUfunction> related = nvbit_get_related_functions(ctx, func);
    related.push_back(func);

    // PCOMM_LAUNCH_DUMP: report the kernel actually being launched, in exactly
    // the form the filter compares against, once per distinct name. This is the
    // input PCOMM_KERNEL_NAME has always needed and had no way to discover —
    // dumping every *instrumented* function instead gives a list that is both
    // enormous (NCCL ships 223 specializations) and truncated by any timeout.
    // Pair it with a filter that matches nothing to skip instrumentation
    // entirely, so the run finishes in seconds.
    if (std::getenv("PCOMM_LAUNCH_DUMP")) {
        static std::unordered_set<std::string>* seen = new std::unordered_set<std::string>();
        std::string                             launched(nvbit_get_func_name(ctx, func));
        launched = launched.substr(0, launched.find('('));
        if (seen->insert(launched).second)
            std::fprintf(stderr, "[pcomm-launch] %s\n", launched.c_str());
    }

    for (auto f : related) {
        std::string func_name(nvbit_get_func_name(ctx, f));
        const bool  want = (G().kernel_filter == "all" ||
                           G().kernel_filter == func_name.substr(0, func_name.find('(')));
        if (want)
            instrument_function_if_needed(ctx, f);

        // The launch id is consumed for every related function whether or not
        // we instrument it, so that filtering cannot shift the id sequence and
        // move existing goldens.
        const uint64_t launch_id = g_grid_launch_id++;

        // Stamp before the launch is enqueued: this is the earliest moment any
        // access from it could occur, which is what makes it a sound lower
        // bound. It is the *enqueue* time, not when the SM actually starts — a
        // deep stream queue makes the window wider than necessary, never wrong.
        //
        // Unconditional, unlike the g_kernel_timing block below: this bound is
        // what stops every access claiming drain time as its own, and that is
        // not a kernel-timing feature to be switched off. The span records are.
        note_launch_time(launch_id);

        // Remember what only the host can see about this launch. Recorded for
        // the launched function only, not for every related one: the related
        // set is other specialisations that share a module, and none of them is
        // what actually ran.
        if (g_kernel_timing && f == func) {
            int dev_id = -1;
            cudaGetDevice(&dev_id);
            if (g_gpu_anchored.insert(dev_id).second)
                sample_gpu_anchor(dev_id);
            LaunchMeta m;
            if (const char* n = nvbit_get_func_name(ctx, f, /*mangled=*/true))
                m.name = n;
            m.grid[0] = geom.gx;
            m.grid[1] = geom.gy;
            m.grid[2] = geom.gz;
            m.block[0] = (uint16_t)geom.bx;
            m.block[1] = (uint16_t)geom.by;
            m.block[2] = (uint16_t)geom.bz;
            m.dev_id = dev_id;
            MutexLock lk(g_span_mutex);
            g_launch_meta.emplace(launch_id, std::move(m));
        }

        // NB: these run for every related function, not just instrumented ones.
        // Skipping them for unfiltered functions was measured (job 43971039)
        // and changes nothing — 436s vs 436s, 438s vs 438s — so the knob was
        // removed rather than left as a dead option. See case 05's meta.json:
        // the cost is NVBit module processing, which we cannot reach from here.
        nvbit_set_at_launch(ctx, f, launch_id);
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
    // Grab the debug counters here too, while we already hold the lock and
    // while the thread is guaranteed to be joinable. Do NOT take g_mutex again
    // later in this function: nvbit_at_ctx_term holds it across the
    // pthread_join of this very thread, so any exit-path lock deadlocks at
    // teardown for every workload size. dbg_host is assigned in
    // nvbit_at_ctx_init before channel_host.init() spawns us, so it is already
    // visible.
    const volatile unsigned long long* dbg_ctrs = nullptr;
    const std::atomic<bool>*           abort_flag = nullptr;
    {
        MutexLock lock(g_mutex);
        CtxState* st = G().ctx_state.at(ctx);
        ch_host = &st->channel_host;
        dbg_ctrs = st->dbg_host;
        abort_flag = &st->recv_abort;
    }

    const long             chan_sz = channel_size();
    char*                  recv_buffer = (char*)malloc(chan_sz);
    std::vector<RawAccess> batch;

    // PCOMM_CHANNEL_DEBUG=1 — diagnostic for the NVBit deadlock (cases 05/07,
    // plans/PLAN-20260727-1022-nvbit-channel-deadlock-done.md). The decisive
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
    if (dbg)
        std::fprintf(stderr, "[pcomm-chan] ctx=%p ring=%ld bytes\n", (void*)ctx, chan_sz);
    unsigned long long recv_calls = 0, empty_calls = 0, total_bytes = 0;
    unsigned long      max_n = 0;
    auto               t_start = std::chrono::steady_clock::now();
    auto               t_last_data = t_start, t_last_report = t_start;

    bool done = false;
    while (!done) {
        // Checked before recv, not only when idle: on a poisoned context recv
        // can keep returning stale ring content forever, so "no data" is not
        // the only shape a stuck channel takes.
        if (abort_flag->load(std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "pcomm: recv thread for ctx=%p asked to stop before the flush token "
                         "(%llu bytes drained) — the rest of this context's device records are "
                         "lost\n",
                         (void*)ctx, total_bytes);
            break;
        }
        uint32_t n = ch_host->recv(recv_buffer, chan_sz);
        ++recv_calls;
        if (n == 0) {
            ++empty_calls;
            if (dbg) {
                auto now = std::chrono::steady_clock::now();
                if (now - t_last_report >= std::chrono::seconds(10)) {
                    t_last_report = now;
                    auto since_data =
                        std::chrono::duration_cast<std::chrono::seconds>(now - t_last_data).count();
                    const volatile unsigned long long* d = dbg_ctrs;
                    std::fprintf(stderr,
                                 "[pcomm-chan] ctx=%p idle: bytes=%llu no_data_for=%llds "
                                 "| dev push=%llu full=%llu flusher=%llu waiter=%llu "
                                 "flushbody=%llu ring=%llu cleared=%llu done=%llu\n",
                                 (void*)ctx, total_bytes, (long long)since_data, d ? d[0] : 0ull,
                                 d ? d[1] : 0ull, d ? d[2] : 0ull, d ? d[3] : 0ull, d ? d[4] : 0ull,
                                 d ? d[5] : 0ull, d ? d[6] : 0ull, d ? d[7] : 0ull);
                }
            }
            continue;
        }
        total_bytes += n;
        if (n > max_n)
            max_n = n;
        t_last_data = std::chrono::steady_clock::now();

        for (uint32_t off = 0; off + sizeof(pcomm_gpu_access_t) <= n;
             off += sizeof(pcomm_gpu_access_t)) {
            auto* ma = (pcomm_gpu_access_t*)&recv_buffer[off];
            if (ma->lane_id == -1) {  // flush token: context is terminating
                done = true;
                break;
            }
            // Kernel entry/exit stamp, not a memory access. Reduced here rather
            // than emitted: a record can only be written once the LAST block of
            // that launch has retired, and nothing in the stream says which
            // block that was — so the reduction is closed at teardown instead.
            // See emit_kernel_spans().
            if (ma->lane_id == PCOMM_SPAN_LANE_ID) {
                const uint64_t id = PCOMM_SPAN_LAUNCH(*ma);
                const uint64_t t = PCOMM_SPAN_TICKS(*ma);
                MutexLock      lk(g_span_mutex);
                LaunchSpan&    s = g_launch_span[id];
                if (PCOMM_SPAN_IS_END(*ma)) {
                    if (t > s.t1)
                        s.t1 = t;
                } else {
                    if (t < s.t0)
                        s.t0 = t;
                    s.blocks_seen++;
                }
                s.dev_id = ma->dev_id;
                continue;
            }

            // The access happened somewhere in [launch, now]: after the kernel
            // that issued it was launched, and before we just dequeued it.
            // Read once per packet, not per lane — one push is one warp, all 32
            // lanes of it from the same launch and the same drain.
            const std::uint64_t t_lo = launch_time(ma->launch_id);
            const std::uint64_t t_hi = pcomm::helpers::TscClock::now();
            if (t_lo == 0)
                ++g_launch_unknown;

            for (int lane = 0; lane < 32; lane++) {
                if (ma->addrs[lane] == 0)
                    continue;
                RawAccess a;
                a.address = ma->addrs[lane];
                a.pc = pcomm::gpumem::pc_handle(ma->instr_index);
                a.size = ma->access_size;
                a.thread = static_cast<uint32_t>(ma->thread_index) + lane;
                a.op = G().id_to_memop[ma->opcode_id];
                a.space = pcomm::core::MemSpace::Global;
                a.run_device = static_cast<pcomm::core::DeviceId>(ma->dev_id);
                a.ticks_lo = t_lo;
                a.ticks_hi = t_hi;
                a.launch = ma->launch_id;
                // mem_device is left unresolved: symmetric-heap ranges are in
                // the alloc registry like any other allocation, so the engine
                // resolves every address the same way.
                batch.push_back(a);
            }
        }

        g_engine->process(batch);
        batch.clear();
    }

    if (dbg) {
        // Device counters on the done line too, not just the idle reports —
        // otherwise a *completing* run reports no push total, and there is
        // nothing to compare a hung run's frozen count against.
        const volatile unsigned long long* d = dbg_ctrs;
        std::fprintf(stderr,
                     "[pcomm-chan] ctx=%p done: recv_calls=%llu empty=%llu bytes=%llu "
                     "max_recv=%lu%s",
                     (void*)ctx, recv_calls, empty_calls, total_bytes, max_n, d ? "" : "\n");
        if (d)
            std::fprintf(stderr,
                         " | dev push=%llu full=%llu flusher=%llu waiter=%llu "
                         "flushbody=%llu ring=%llu cleared=%llu done=%llu\n",
                         d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    g_engine->drain_thread();  // buffers never auto-drain (leaked TLS)
    free(recv_buffer);
    return nullptr;
}

}  // namespace

// nvshmem_malloc interposition used to live here. It moved to the engine
// (libpcomm/src/engine.cpp) because "what did the application allocate" must
// not depend on which provider ran: an mlx5dv-only capture of the same NVSHMEM
// workload held none of the buffers the program asked for, and NVBit costs
// ~435 s of module lifting to find out. See core/object_registry.hpp.
//
// It was also in the wrong file even within gpumem — this one's header reserves
// it for vendor instrumentation types, and interposing a third-party library is
// not that.

__global__ void pcomm_flush_channel(ChannelDev* ch_dev) {
    pcomm_gpu_access_t ma;
    memset(&ma, 0, sizeof(ma));
    ma.lane_id = -1;
    ch_dev->push(&ma, sizeof(pcomm_gpu_access_t));
    ch_dev->flush();
}

// Close the per-launch reduction and hand every kernel to the engine.
//
// DEFERRED TO TEARDOWN on purpose. A launch's window is only known once its
// last block has retired, and nothing in the channel says which block that was
// — there is no "kernel done" marker to emit on. Holding the map instead costs
// ~64 bytes per launch, which is nothing against the ring it shares a process
// with, and makes the emitted span exact rather than a guess about arrival
// order.
//
// A second anchor per GPU is taken here, so the domain has the two points
// pcomm-read needs to fit rate as well as offset. Without it the fit falls back
// to 1.0 ns/tick and the measured 5.7-13.9 ppm drift becomes skew.
void emit_kernel_spans() {
    if (!g_kernel_timing)
        return;
    // NB: the closing anchors are NOT sampled here. This runs after the last
    // context has been destroyed, and sample_gpu_anchor launches a kernel — it
    // would fail, or worse resurrect a context at exit. They are taken in
    // nvbit_at_ctx_term instead, while the context is still alive.
    MutexLock   lk(g_span_mutex);
    std::size_t emitted = 0, unmatched = 0;
    for (const auto& [id, s] : g_launch_span) {
        // A span needs both ends. An entry with no exit means the kernel was
        // still running at teardown, or its exit records were lost with the
        // context — either way the window is unknown and inventing one would be
        // worse than the absence.
        if (s.t0 == UINT64_MAX || s.t1 == 0 || s.t1 < s.t0) {
            unmatched++;
            continue;
        }
        auto              it = g_launch_meta.find(id);
        const LaunchMeta* m = it == g_launch_meta.end() ? nullptr : &it->second;
        const int         dev = m && m->dev_id >= 0 ? m->dev_id : s.dev_id;
        pcomm_engine_record_kernel(
            s.t0, s.t1, (uint16_t)(pcomm::core::kDomainGpuTimerBase + (dev < 0 ? 0 : dev)),
            m ? m->name.c_str() : nullptr,
            /*correlation=*/id,
            // No stream: NVBit's launch callback does not carry one for every
            // entry point, and a fabricated 0 would claim every kernel shared a
            // lane. 0 here means "unknown", as it does for correlation.
            /*stream=*/0, (uint16_t)(dev < 0 ? 0 : dev),
            (uint8_t)pcomm::core::KernelSource::NvbitGlobaltimer, m ? m->grid : nullptr,
            m ? m->block : nullptr);
        emitted++;
    }
    if (std::getenv("PCOMM_VERBOSE") || unmatched)
        std::fprintf(stderr, "[pcomm-nvbit] kernel spans: %zu emitted, %zu incomplete\n", emitted,
                     unmatched);
}

// --- NVBit entry points ------------------------------------------------------

void nvbit_at_init() {
    setenv("CUDA_MANAGED_FORCE_DEVICE_ALLOC", "1", 1);
    if (const char* k = std::getenv("PCOMM_KERNEL_NAME"))
        G().kernel_filter = k;
    // "Does this run want kernel times", and nothing about CUPTI: the stamp
    // this gates is PTX %globaltimer, which shares no state with the activity
    // API. Until 2026-08-17 this read the launcher's CUPTI guard by the same
    // name, so `-p p2p` — the case that has no CUPTI and therefore needs this
    // path most — turned it off. engine_abi.h has what that cost.
    //
    // Read ONCE: the instrumentation loop runs per instruction over modules
    // with hundreds of specialisations, so a getenv in it would be a real cost.
    g_kernel_timing = pcomm_engine_kernel_timing_wanted() != 0;
    g_engine = new pcomm::gpumem::Engine();
}

void nvbit_tool_init(CUcontext) {}
void nvbit_at_graph_node_launch(CUcontext, CUfunction, CUstream, uint64_t) {}

void nvbit_at_cuda_event(CUcontext ctx, int is_exit, nvbit_api_cuda_t cbid, const char* /*name*/,
                         void* params, CUresult*) {
    MutexLock lock(g_mutex);
    if (g_skip_callback)
        return;
    g_skip_callback = true;

    if (!is_exit && (cbid == API_CUDA_cuLaunchKernel_ptsz || cbid == API_CUDA_cuLaunchKernel)) {
        auto* p = (cuLaunchKernel_params*)params;
        handle_launch(
            ctx, p->f,
            {p->gridDimX, p->gridDimY, p->gridDimZ, p->blockDimX, p->blockDimY, p->blockDimZ});
    } else if (!is_exit && cbid == API_CUDA_cuLaunchKernelEx) {
        auto* p = (cuLaunchKernelEx_params*)params;
        // cuLaunchKernelEx carries the grid in a config struct instead.
        handle_launch(ctx, p->f,
                      {p->config->gridDimX, p->config->gridDimY, p->config->gridDimZ,
                       p->config->blockDimX, p->config->blockDimY, p->config->blockDimZ});
    } else if (!is_exit && (cbid == API_CUDA_cuLaunchCooperativeKernel ||
                            cbid == API_CUDA_cuLaunchCooperativeKernel_ptsz)) {
        auto* p = (cuLaunchCooperativeKernel_params*)params;
        handle_launch(
            ctx, p->f,
            {p->gridDimX, p->gridDimY, p->gridDimZ, p->blockDimX, p->blockDimY, p->blockDimZ});
    } else if (is_exit && cbid == API_CUDA_cuMemAlloc_v2) {
        auto* p = (cuMemAlloc_v2_params*)params;
        handle_alloc(*p->dptr, p->bytesize);
    } else if (is_exit && cbid == API_CUDA_cuMemAlloc) {
        auto* p = (cuMemAlloc_params*)params;
        handle_alloc(*p->dptr, p->bytesize);
    } else if (is_exit && cbid == API_CUDA_cuMemAllocManaged) {
        auto* p = (cuMemAllocManaged_params*)params;
        handle_alloc(*p->dptr, p->bytesize);
    } else if (!is_exit && (cbid == API_CUDA_cuMemFree_v2 || cbid == API_CUDA_cuMemFree)) {
        // On ENTRY, not exit: the pointer is the thing being closed, and after
        // the call returns the address can already have been handed to another
        // allocation. Closing first keeps the record ordered with reality.
        //
        // cuMemFree rather than cudaFree because the allocation side hooks
        // cuMemAlloc: cudaMalloc funnels into it, so the driver layer is where
        // both halves belong and a runtime-level free hook would miss every
        // driver-API allocation.
        const uint64_t dptr = cbid == API_CUDA_cuMemFree_v2
                                  ? (uint64_t)((cuMemFree_v2_params*)params)->dptr
                                  : (uint64_t)((cuMemFree_params*)params)->dptr;
        if (g_engine && dptr)
            g_engine->on_free(dptr);
    } else if (is_exit && vmm_trace_on() && cbid == API_CUDA_cuMemCreate) {
        auto* p = (cuMemCreate_params*)params;
        std::fprintf(stderr, "[pcomm-vmm] create handle=%llu size=%zu dev=%d type=%d\n",
                     p->handle ? (unsigned long long)*p->handle : 0ull, (size_t)p->size,
                     p->prop ? (int)p->prop->location.id : -1,
                     p->prop ? (int)p->prop->requestedHandleTypes : -1);
    } else if (is_exit && cbid == API_CUDA_cuMemExportToShareableHandle) {
        auto* p = (cuMemExportToShareableHandle_params*)params;
        // 0x8 = CU_MEM_HANDLE_TYPE_FABRIC, the only type that is an identity.
        if (p->shareableHandle && (unsigned)p->handleType == 0x8)
            G().fabric_of[(unsigned long long)p->handle] = {fabric_hex(p->shareableHandle), true};
        if (vmm_trace_on()) {
            // For a POSIX fd the out-param is an int; for a fabric handle it is a
            // 64-byte blob. Reading it as an fd is only meaningful in the first case,
            // which is exactly what the type tells us.
            const long long fd = p->shareableHandle ? (long long)*(int*)p->shareableHandle : -1;
            vmm_trace_fd("export", fd, (unsigned)p->handleType);
        }
    } else if (is_exit && cbid == API_CUDA_cuMemImportFromShareableHandle) {
        auto* p = (cuMemImportFromShareableHandle_params*)params;
        if (p->handle && p->osHandle && (unsigned)p->shHandleType == 0x8)
            G().fabric_of[(unsigned long long)*p->handle] = {fabric_hex(p->osHandle), false};
        if (vmm_trace_on()) {
            const long long fd = (long long)(intptr_t)p->osHandle;
            vmm_trace_fd("import", fd, (unsigned)p->shHandleType);
        }
    } else if (is_exit && vmm_trace_on() && cbid == API_CUDA_cuIpcGetMemHandle) {
        std::fprintf(stderr, "[pcomm-vmm] ipc_get\n");
        std::fflush(stderr);
    } else if (is_exit && vmm_trace_on() && cbid == API_CUDA_cuIpcOpenMemHandle) {
        auto* p = (cuIpcOpenMemHandle_params*)params;
        std::fprintf(stderr, "[pcomm-vmm] ipc_open ptr=0x%llx\n",
                     p->pdptr ? (unsigned long long)*p->pdptr : 0ull);
        std::fflush(stderr);
    } else if (is_exit && cbid == API_CUDA_cuMemMap) {
        if (vmm_trace_on()) {
            auto* q = (cuMemMap_params*)params;
            std::fprintf(stderr, "[pcomm-vmm] map ptr=0x%llx size=%zu offset=%zu handle=%llu\n",
                         (unsigned long long)q->ptr, (size_t)q->size, (size_t)q->offset,
                         (unsigned long long)q->handle);
            std::fflush(stderr);
        }
        // Record the extent, nothing more. Who owns each of these is decided at
        // the next launch, by asking CUDA and NVSHMEM — see
        // refresh_mapped_regions. cuMemMap is only the source of exact sizes,
        // and contributes no ordering, no index and no count.
        auto* p = (cuMemMap_params*)params;
        auto& g = G();
        if (g.mapped_bases_seen.insert((uint64_t)p->ptr).second) {
            // Carry the allocation's identity onto the mapping, when there is
            // one. This is the only moment both are in hand: the export/import
            // hook knows the identity, cuMemMap knows the local address.
            std::string hex;
            bool        exporter = false;
            if (auto it = g.fabric_of.find((unsigned long long)p->handle);
                it != g.fabric_of.end()) {
                hex = it->second.first;
                exporter = it->second.second;
            }
            g.mapped_regions.push_back({(uint64_t)p->ptr, (uint64_t)p->size, std::move(hex),
                                        (uint64_t)p->offset, exporter});
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
    // Diagnostic counters (PCOMM_CHANNEL_DEBUG): host-mapped so the recv thread
    // can watch where the device stops. See channel.hpp pcomm_bump slots.
    if (std::getenv("PCOMM_CHANNEL_DEBUG")) {
        unsigned long long* h = nullptr;
        if (cudaHostAlloc((void**)&h, 8 * sizeof(unsigned long long), cudaHostAllocMapped) ==
            cudaSuccess) {
            memset(h, 0, 8 * sizeof(unsigned long long));
            unsigned long long* d = nullptr;
            if (cudaHostGetDevicePointer((void**)&d, h, 0) == cudaSuccess) {
                state->dbg_host = h;
                state->channel_dev->pcomm_dbg = d;
            }
        }
    }
    state->channel_host.init((int)G().ctx_state.size() - 1, channel_size(), state->channel_dev,
                             recv_thread_fun, ctx);
    nvbit_set_tool_pthread(state->channel_host.get_thread());
}

void nvbit_at_ctx_term(CUcontext ctx) {
    MutexLock lock(g_mutex);
    g_skip_callback = true;
    CtxState* state = G().ctx_state.at(ctx);

    // Consume any error latched BEFORE us. cudaGetLastError returns the last
    // error from this thread, not the last one from the two lines above it, so
    // asserting on it here charged us for anything the application or NVSHMEM
    // had already done. That is how case 13 died: exit 134, SIGABRT out of a
    // profiler's teardown, with the capture lost.
    if (const cudaError_t stale = cudaGetLastError(); stale != cudaSuccess)
        std::fprintf(stderr, "pcomm: cuda error already pending at ctx term (not ours): %s\n",
                     cudaGetErrorString(stale));

    pcomm_flush_channel<<<1, 1>>>(state->channel_dev);
    cudaError_t flush = cudaDeviceSynchronize();
    if (flush == cudaSuccess)
        flush = cudaGetLastError();

    // REPORT, never abort. This runs at context teardown of the application we
    // are profiling: aborting here kills the app at exit and takes the whole
    // capture with it, which is the worst possible response to "the last flush
    // may have dropped some records". The remaining teardown below is what
    // writes the streams, so it has to run either way.
    if (flush != cudaSuccess) {
        std::fprintf(stderr,
                     "pcomm: channel flush failed at ctx term: %s — records still in the "
                     "device buffer are lost, the rest of the capture is intact\n",
                     cudaGetErrorString(flush));
        // The flush kernel did not run, so it never wrote the token the recv
        // thread waits for. Without this the destroy() below joins a thread
        // that will never return: replacing the old abort() with a report
        // turned a fast crash into a hung allocation (job 44458142, killed at
        // the walltime with two later cases never started).
        state->recv_abort.store(true, std::memory_order_relaxed);
    }

    // The CLOSING clock anchor for every GPU this run touched, taken HERE
    // because this is the last moment the context is alive — emit_kernel_spans
    // runs after the context is gone and could not launch the sampling kernel.
    // Two anchors far apart per domain is what lets pcomm-read fit the rate as
    // well as the offset, and the rate is the part that matters: globaltimer
    // drifts 5.7-13.9 ppm against the wall clock, which over a 7 s capture is
    // ~97 us of skew against kernels lasting tens of microseconds.
    //
    // ONCE PER RUN, not once per context. This loop closes EVERY device, so in
    // a process with four of them (case 03) running it at all four ctx terms
    // would sample sixteen times — and the last three passes would launch the
    // sampling kernel into contexts already destroyed by the earlier passes.
    // The first ctx term is the one moment every context is still alive, so it
    // is the right one to close them all at.
    // Driven off g_anchor_buf, NOT off g_gpu_anchored, and the reason is the one
    // in sample_gpu_anchor's comment: this runs during exit, and g_gpu_anchored
    // is a std::unordered_set whose destructor may already have run. It survived
    // job 44713195 on destruction-order luck — being a load-time global it is
    // torn down late — which is not a property to rely on. g_anchor_buf is a POD
    // array and a non-null slot means that device was anchored, so it answers
    // the same question with nothing to destroy.
    static bool closed = false;
    if (g_kernel_timing && flush == cudaSuccess && !closed) {
        closed = true;
        for (int dev = 0; dev < kMaxAnchoredDevices; ++dev)
            if (g_anchor_buf[dev])
                sample_gpu_anchor(dev);
    }

    state->channel_host.destroy(false);
    cudaFree(state->channel_dev);
    g_skip_callback = false;
    delete state;
    G().ctx_state.erase(ctx);

    // Last context gone: flush everything now, while the C++ runtime (Tracer,
    // sinks, intern tables) is still alive. nvbit_at_term is too late.
    if (G().ctx_state.empty() && g_engine && !g_shutdown_done) {
        g_shutdown_done = true;
        emit_kernel_spans();  // before shutdown(): it needs the recorder alive
        // Windows with no lower bound are a real loss of information, so say so
        // rather than letting them look like ordinary records. Non-zero means
        // launches aged out of the ring (raise kLaunchRing) or a launch path we
        // do not intercept — CUDA-graph node launches are the known one,
        // nvbit_at_graph_node_launch is still a stub.
        if (g_launch_unknown)
            std::fprintf(stderr,
                         "[pcomm] %llu warp-packets had no launch time — their accesses are "
                         "recorded with an open lower bound (window [0, drain])\n",
                         g_launch_unknown);
        g_engine->shutdown();
    }
}

void nvbit_at_term() {
    // Runs during __cxa_finalize — statics may be gone; nothing safe to do.
}
