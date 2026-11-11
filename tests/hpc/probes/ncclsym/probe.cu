// PROBE — is NCCL's symmetric window actually symmetric, and does this machine
// support fabric handles?
//
// Two questions, one allocation each, because both decide a design and neither
// is worth a build to guess at.
//
//   1. NCCL 2.27+ has ncclCommWindowRegister(..., NCCL_WIN_COLL_SYMMETRIC).
//      If a window registered that way lands at the SAME virtual address on
//      every rank, then it has the one property NVSHMEM's heap has and comshu's
//      peer attribution already needs — object_access translates an actor's
//      object id into the owner's namespace by matching (base, size), which
//      works precisely when the buffer is symmetric. If the addresses differ,
//      the window is symmetric in NCCL's bookkeeping but not in the VA space,
//      and it buys us nothing.
//
//   2. CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED (128). The POSIX-fd
//      shareable handle carries no allocation identity — measured, job 44705498,
//      every export and import fstat'd to the same driver-node inode. A fabric
//      handle would be globally meaningful and make that join trivial, so
//      whether this hardware can produce one decides if that route exists here
//      at all.
//
// Prints one line per rank; the comparison is done by eye (or by the sbatch's
// sort -u) across ranks.
#include <cstdio>
#include <cuda.h>
#include <cuda_runtime.h>
#include <mpi.h>
#include <nccl.h>

#define CK(x)                                                                                      \
    do {                                                                                           \
        cudaError_t e = (x);                                                                       \
        if (e != cudaSuccess) {                                                                    \
            std::fprintf(stderr, "cuda %s @%d\n", cudaGetErrorString(e), __LINE__);                \
            MPI_Abort(MPI_COMM_WORLD, 1);                                                          \
        }                                                                                          \
    } while (0)
#define NK(x)                                                                                      \
    do {                                                                                           \
        ncclResult_t r = (x);                                                                      \
        if (r != ncclSuccess) {                                                                    \
            std::fprintf(stderr, "nccl %s @%d\n", ncclGetErrorString(r), __LINE__);                \
            MPI_Abort(MPI_COMM_WORLD, 2);                                                          \
        }                                                                                          \
    } while (0)

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int local = 0;
    if (const char* e = getenv("OMPI_COMM_WORLD_LOCAL_RANK"))
        local = atoi(e);
    CK(cudaSetDevice(local));

    // Q2 first: it needs no NCCL and cannot fail the run.
    int      fabric = -1;
    CUdevice dev;
    if (cuDeviceGet(&dev, local) == CUDA_SUCCESS)
        cuDeviceGetAttribute(&fabric, (CUdevice_attribute)128 /* HANDLE_TYPE_FABRIC_SUPPORTED */,
                             dev);
    std::printf("[ncclsym] rank=%d dev=%d fabric_supported=%d\n", rank, local, fabric);
    std::fflush(stdout);

    ncclUniqueId id;
    if (rank == 0)
        NK(ncclGetUniqueId(&id));
    MPI_Bcast(&id, sizeof(id), MPI_BYTE, 0, MPI_COMM_WORLD);
    ncclComm_t comm;
    NK(ncclCommInitRank(&comm, size, id, rank));

    const size_t N = 1u << 20;  // 1 MiB, 4096-aligned as NCCL_WIN_REQUIRED_ALIGNMENT wants

    // ncclMemAlloc, not cudaMalloc: the window API expects memory it can place,
    // and this is also the door a provider would hook to learn the buffer.
    void* buf = nullptr;
    NK(ncclMemAlloc(&buf, N));
    std::printf("[ncclsym] rank=%d ncclMemAlloc=%p\n", rank, buf);
    std::fflush(stdout);

    ncclWindow_t win = nullptr;
    ncclResult_t wr = ncclCommWindowRegister(comm, buf, N, &win, NCCL_WIN_COLL_SYMMETRIC);
    std::printf("[ncclsym] rank=%d windowRegister=%s win=%p\n", rank,
                wr == ncclSuccess ? "ok" : ncclGetErrorString(wr), (void*)win);
    std::fflush(stdout);

    // Exercise it, so any peer traffic that a symmetric window enables actually
    // happens and a capture of this probe has something to attribute.
    cudaStream_t s;
    CK(cudaStreamCreate(&s));
    for (int i = 0; i < 5; ++i)
        NK(ncclAllReduce(buf, buf, N / sizeof(float), ncclFloat, ncclSum, comm, s));
    CK(cudaStreamSynchronize(s));

    if (wr == ncclSuccess)
        NK(ncclCommWindowDeregister(comm, win));
    NK(ncclCommDestroy(comm));
    MPI_Finalize();
    return 0;
}
