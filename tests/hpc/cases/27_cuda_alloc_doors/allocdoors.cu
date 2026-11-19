// Case 27 — every route by which a program gets CUDA memory, once each, at a
// size no other route uses, so an assertion can find each one's object without
// knowing an address.
//
//   allocdoors <malloc> <managed> <async> <host> <hostalloc> <driver> <dlsym>
//              <loop_iters> <loop_size> <pitch_width> <pitch_height>
//
// The routes, and the engine door each is expected to arrive through
// (PLAN-20261118-1930; libcomshu/src/cuda_alloc_doors.cpp):
//
//   cudaMalloc / cudaMallocPitch / cudaMallocManaged /
//   cudaMallocAsync / cudaMallocHost / cudaHostAlloc   the CUDA RUNTIME — static or
//                                                      shared, the job builds both —
//                                                      which asks the driver via
//                                                      cuGetProcAddress: the CELL door
//   cuMemAlloc / cuMemFree, cuMem{Create,Map,Unmap}    driver API, linked -lcuda:
//                                                      the LINK door
//   dlsym(libcuda, "cuMemAlloc_v2")                    the DLSYM door
//
// And three behaviours the engine must get right, not just see:
//   - two VMM chunks mapped into one reservation and unmapped by ONE cuMemUnmap:
//     both objects close, and only those two;
//   - a chunk EXPORTED as a file descriptor, imported back and mapped — the way
//     NCCL and NVSHMEM map a peer's buffer: that mapping is NOT an object. The
//     memory is the exporter's; recording the importer's window onto it as the
//     importer's buffer is what credited rank 0's writes into GPU 1 to rank 0
//     (nccl_p2p, MN5 49207744). The one process plays both sides here;
//   - cudaFree of the same pointer twice: one close, not two;
//   - an allocate/free loop at one size: one open and one close per iteration,
//     and a registry that revives its entry rather than growing per call.
//
// Prints what it did, so the log carries the oracle beside the assertions.

#include <cuda.h>
#include <cuda_runtime.h>
#include <dlfcn.h>

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CK(x)                                                                                      \
    do {                                                                                           \
        cudaError_t e_ = (x);                                                                      \
        if (e_ != cudaSuccess) {                                                                   \
            std::fprintf(stderr, "%s:%d %s: %s\n", __FILE__, __LINE__, #x,                         \
                         cudaGetErrorString(e_));                                                  \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)
#define DK(x)                                                                                      \
    do {                                                                                           \
        CUresult r_ = (x);                                                                         \
        if (r_ != CUDA_SUCCESS) {                                                                  \
            const char* s_ = "?";                                                                  \
            cuGetErrorString(r_, &s_);                                                             \
            std::fprintf(stderr, "%s:%d %s: %s\n", __FILE__, __LINE__, #x, s_);                    \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)

static size_t arg(char** argv, int i) {
    return std::strtoull(argv[i], nullptr, 10);
}

int main(int argc, char** argv) {
    if (argc != 12) {
        std::fprintf(stderr,
                     "usage: %s malloc managed async host hostalloc driver dlsym "
                     "loop_iters loop_size pitch_width pitch_height\n",
                     argv[0]);
        return 1;
    }
    const size_t s_malloc = arg(argv, 1), s_managed = arg(argv, 2), s_async = arg(argv, 3),
                 s_host = arg(argv, 4), s_hostalloc = arg(argv, 5), s_driver = arg(argv, 6),
                 s_dlsym = arg(argv, 7), loop_iters = arg(argv, 8), loop_size = arg(argv, 9),
                 pitch_w = arg(argv, 10), pitch_h = arg(argv, 11);

    CK(cudaSetDevice(0));
    CK(cudaFree(nullptr));  // create the context before anything is measured

    // THREE PHASES, and the order is the oracle's. A MemObject's id is interned
    // from its BASE, so two allocations that ever share an address share an id —
    // and a close could not then be credited to one route. So every route's
    // buffer is allocated and KEPT ALIVE (live buffers cannot share an address),
    // then the loop runs beside them, then everything is freed. Each route ends
    // up with an id of its own; only the loop reuses one, which is its point.

    // --- 1. allocate, through every route ---------------------------------------
    // the runtime (cell door)
    void* a = nullptr;
    CK(cudaMalloc(&a, s_malloc));
    std::printf("route=cudaMalloc size=%zu addr=%p\n", s_malloc, a);

    void*  p = nullptr;
    size_t pitch = 0;
    CK(cudaMallocPitch(&p, &pitch, pitch_w, pitch_h));
    std::printf("route=cudaMallocPitch size=%zu pitch=%zu addr=%p\n", pitch * pitch_h, pitch, p);

    void* m = nullptr;  // never freed: a buffer alive at exit is legal
    CK(cudaMallocManaged(&m, s_managed));
    std::printf("route=cudaMallocManaged size=%zu addr=%p\n", s_managed, m);

    cudaStream_t st;
    CK(cudaStreamCreate(&st));
    void* as = nullptr;
    CK(cudaMallocAsync(&as, s_async, st));
    CK(cudaStreamSynchronize(st));
    std::printf("route=cudaMallocAsync size=%zu addr=%p\n", s_async, as);

    void *h1 = nullptr, *h2 = nullptr;
    CK(cudaMallocHost(&h1, s_host));
    CK(cudaHostAlloc(&h2, s_hostalloc, cudaHostAllocDefault));
    std::printf("route=cudaMallocHost size=%zu addr=%p\n", s_host, h1);
    std::printf("route=cudaHostAlloc size=%zu addr=%p\n", s_hostalloc, h2);

    // the driver API, linked -lcuda (link door)
    CUdeviceptr d = 0;
    DK(cuMemAlloc(&d, s_driver));
    std::printf("route=cuMemAlloc size=%zu addr=0x%llx\n", s_driver, (unsigned long long)d);

    // the driver by dlsym (dlsym door)
    void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_NOLOAD);
    using AllocFn = CUresult (*)(CUdeviceptr*, size_t);
    using FreeFn = CUresult (*)(CUdeviceptr);
    auto fa = lib ? reinterpret_cast<AllocFn>(dlsym(lib, "cuMemAlloc_v2")) : nullptr;
    auto ff = lib ? reinterpret_cast<FreeFn>(dlsym(lib, "cuMemFree_v2")) : nullptr;
    if (!fa || !ff) {
        std::fprintf(stderr, "dlsym of libcuda's cuMemAlloc_v2/cuMemFree_v2 failed\n");
        return 2;
    }
    CUdeviceptr dd = 0;
    DK(fa(&dd, s_dlsym));
    std::printf("route=dlsym size=%zu addr=0x%llx\n", s_dlsym, (unsigned long long)dd);

    // VMM: two maps into one reservation (link door). Access is granted AFTER
    // the maps, the order NCCL and PyTorch use — the engine must name the device
    // from the handle, because the address cannot answer before this call.
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = 0;
    size_t gran = 0;
    DK(cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    CUdeviceptr base = 0;
    DK(cuMemAddressReserve(&base, 2 * gran, 0, 0, 0));
    CUmemGenericAllocationHandle hv[2];
    for (int i = 0; i < 2; ++i) {
        DK(cuMemCreate(&hv[i], gran, &prop, 0));
        DK(cuMemMap(base + i * gran, gran, 0, hv[i], 0));
        std::printf("route=cuMemMap size=%zu addr=0x%llx\n", gran,
                    (unsigned long long)(base + i * gran));
    }
    CUmemAccessDesc acc = {};
    acc.location = prop.location;
    acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    DK(cuMemSetAccess(base, 2 * gran, &acc, 1));

    // An imported mapping: create an exportable chunk, export it as a POSIX fd,
    // import the fd and map THAT handle. Only the import is mapped, so the
    // CuMemMap object count stays at the two above if and only if the doors
    // leave an imported mapping alone.
    CUmemAllocationProp xprop = prop;
    xprop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    CUmemGenericAllocationHandle hx = 0, himp = 0;
    DK(cuMemCreate(&hx, gran, &xprop, 0));
    int fd = -1;
    DK(cuMemExportToShareableHandle(&fd, hx, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0));
    DK(cuMemImportFromShareableHandle(&himp, reinterpret_cast<void*>(static_cast<intptr_t>(fd)),
                                      CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR));
    CUdeviceptr xbase = 0;
    DK(cuMemAddressReserve(&xbase, gran, 0, 0, 0));
    DK(cuMemMap(xbase, gran, 0, himp, 0));
    DK(cuMemSetAccess(xbase, gran, &acc, 1));
    std::printf("route=imported-map size=%zu addr=0x%llx (must NOT be an object)\n", gran,
                (unsigned long long)xbase);

    // --- 2. the loop, beside every live buffer ------------------------------------
    for (size_t i = 0; i < loop_iters; ++i) {
        void* q = nullptr;
        CK(cudaMalloc(&q, loop_size));
        CK(cudaFree(q));
    }
    std::printf("route=loop iters=%zu size=%zu\n", loop_iters, loop_size);

    // --- 3. free everything (but the managed buffer) -------------------------------
    CK(cudaFree(a));
    const cudaError_t again = cudaFree(a);  // the double free: one close, not two
    std::printf("second cudaFree of the same pointer: %s\n", cudaGetErrorString(again));
    (void)cudaGetLastError();  // not sticky; clear it so the next CK is honest
    CK(cudaFree(p));
    CK(cudaFreeAsync(as, st));
    CK(cudaStreamSynchronize(st));
    CK(cudaFreeHost(h1));
    CK(cudaFreeHost(h2));
    DK(cuMemFree(d));
    DK(ff(dd));
    DK(cuMemUnmap(base, 2 * gran));  // ONE call, two mappings
    for (int i = 0; i < 2; ++i)
        DK(cuMemRelease(hv[i]));
    DK(cuMemAddressFree(base, 2 * gran));
    DK(cuMemUnmap(xbase, gran));
    DK(cuMemRelease(himp));
    DK(cuMemRelease(hx));
    DK(cuMemAddressFree(xbase, gran));
    close(fd);

    CK(cudaStreamDestroy(st));
    std::printf("oracle: 1 cudaMalloc, 1 pitch, 1 managed (unfreed), 1 async, 2 pinned host, "
                "1 cuMemAlloc, 1 dlsym, 2 cuMemMap (1 unmap), 1 imported map (no object), "
                "%zu loop allocations\n",
                loop_iters);
    return 0;
}
