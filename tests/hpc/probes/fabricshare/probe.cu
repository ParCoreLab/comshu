// PROBE / VALIDATION — share GPU memory between two processes with a FABRIC
// handle, and write into the peer's mapping from a kernel.
//
// WHY IT IS SYNTHETIC. PeerMapping (record 526) exists to join an actor's
// mapping of peer memory to the owner's allocation, and a fabric handle is the
// only identity that can carry that join: its 64 bytes are globally meaningful,
// while the POSIX fd this stack's NCCL actually uses fstat'd to one driver-node
// inode for all 48 exports and imports (MN5 job 44705498). NCCL selects fabric
// only for MNNVL topologies and offers no knob for the handle type, so on MN5
// no real workload will produce one — but the hardware can
// (FABRIC_SUPPORTED=1 on all four H100s, job 44709082). This asks for one
// directly, so the record and its join can be validated where they cannot yet
// be observed.
//
// WHAT A PASS LOOKS LIKE. Both ranks emit a peer_mapping row; the two rows
// carry the SAME `allocation` string; one has exporter=1 and the other 0; and
// the writing rank's gpu_access rows land inside its imported mapping.
//
// A CLEAN FAILURE IS ALSO A RESULT. Fabric import across processes needs the
// IMEX daemon / fabric manager configured, and MN5 may not have it. That
// surfaces here as a named CUDA error on the import rather than as a mystery,
// which is why every driver call is checked and reported by name.
#include <cstdio>
#include <cstring>
#include <cuda.h>
#include <mpi.h>

#define DK(x)                                                                                      \
    do {                                                                                           \
        CUresult r_ = (x);                                                                         \
        if (r_ != CUDA_SUCCESS) {                                                                  \
            const char* n_ = nullptr;                                                              \
            cuGetErrorName(r_, &n_);                                                               \
            std::printf("[fabric] rank=%d FAIL %s -> %s\n", g_rank, #x, n_ ? n_ : "?");            \
            std::fflush(stdout);                                                                   \
            MPI_Abort(MPI_COMM_WORLD, 3);                                                          \
        }                                                                                          \
    } while (0)

static int g_rank = 0;

__global__ void poke(int* dst, int n, int v) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        dst[i] = v;
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size != 2) {
        if (!g_rank)
            std::fprintf(stderr, "fabricshare: needs exactly 2 ranks\n");
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    DK(cuInit(0));
    CUdevice dev;
    DK(cuDeviceGet(&dev, g_rank));
    CUcontext ctx;
    DK(cuCtxCreate(&ctx, 0, dev));

    int fabric = 0;
    DK(cuDeviceGetAttribute(&fabric, (CUdevice_attribute)128, dev));
    std::printf("[fabric] rank=%d dev=%d fabric_supported=%d\n", g_rank, g_rank, fabric);
    std::fflush(stdout);
    if (!fabric) {
        std::printf("[fabric] rank=%d SKIP — device cannot export fabric handles\n", g_rank);
        MPI_Finalize();
        return 0;
    }

    // Ask for a FABRIC handle explicitly. This is the whole point: the default
    // on this stack is a POSIX fd, which carries no allocation identity.
    CUmemAllocationProp prop{};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = g_rank;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC;

    size_t gran = 0;
    DK(cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_RECOMMENDED));
    const size_t N = gran;  // exactly one granule, so offsets stay legible

    CUmemGenericAllocationHandle mine;
    DK(cuMemCreate(&mine, N, &prop, 0));

    CUdeviceptr myptr;
    DK(cuMemAddressReserve(&myptr, N, 0, 0, 0));
    DK(cuMemMap(myptr, N, 0, mine, 0));
    CUmemAccessDesc acc{};
    acc.location = prop.location;
    acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    DK(cuMemSetAccess(myptr, N, &acc, 1));
    std::printf("[fabric] rank=%d own_map=0x%llx size=%zu\n", g_rank, (unsigned long long)myptr, N);
    std::fflush(stdout);

    // Export, swap the 64 bytes, import the peer's.
    CUmemFabricHandle out{};
    DK(cuMemExportToShareableHandle(&out, mine, CU_MEM_HANDLE_TYPE_FABRIC, 0));
    CUmemFabricHandle in{};
    const int         peer = 1 - g_rank;
    MPI_Sendrecv(&out, sizeof(out), MPI_BYTE, peer, 7, &in, sizeof(in), MPI_BYTE, peer, 7,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    CUmemGenericAllocationHandle theirs;
    DK(cuMemImportFromShareableHandle(&theirs, &in, CU_MEM_HANDLE_TYPE_FABRIC));

    CUdeviceptr peerptr;
    DK(cuMemAddressReserve(&peerptr, N, 0, 0, 0));
    DK(cuMemMap(peerptr, N, 0, theirs, 0));
    // Access is granted to MY device: my kernel is the one reading/writing it.
    CUmemAccessDesc pacc{};
    pacc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    pacc.location.id = g_rank;
    pacc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    DK(cuMemSetAccess(peerptr, N, &pacc, 1));
    std::printf("[fabric] rank=%d peer_map=0x%llx (peer=%d)\n", g_rank, (unsigned long long)peerptr,
                peer);
    std::fflush(stdout);

    // Write into the PEER's memory, which is the access comshu has to attribute.
    const int elems = (int)(N / sizeof(int));
    poke<<<(elems + 255) / 256, 256>>>((int*)peerptr, elems, g_rank + 1);
    DK(cuCtxSynchronize());
    MPI_Barrier(MPI_COMM_WORLD);
    std::printf("[fabric] rank=%d wrote %d ints into the peer mapping\n", g_rank, elems);
    std::fflush(stdout);

    DK(cuMemUnmap(peerptr, N));
    DK(cuMemAddressFree(peerptr, N));
    DK(cuMemRelease(theirs));
    DK(cuMemUnmap(myptr, N));
    DK(cuMemAddressFree(myptr, N));
    DK(cuMemRelease(mine));
    MPI_Finalize();
    return 0;
}
