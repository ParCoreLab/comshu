// gin_putring — the put-ring over NCCL's GPU-initiated networking (GIN).
//
//   mpirun -np <P> gin_putring [chunk_bytes=128] [puts_per_iter=1024] [iters=20]
//
// Every iteration each rank fills its send window with a pattern unique to
// (rank, iteration, word) and puts all of it to its right neighbour's receive
// window from ONE device kernel through ncclGin: `puts_per_iter` puts of
// `chunk_bytes`, one per thread, so a whole grid of them can be in flight at
// once. Each put increments GIN signal 0 on the neighbour. The kernel then
// waits until its own signal 0 has counted every put its left neighbour has
// made so far, and the host checks every word of the receive window against the
// left neighbour's pattern for that iteration.
//
// A put that is missing, short, or overwritten by a stale one fails the job
// non-zero, so "OK" means every byte of every iteration arrived — the same
// promise tests/hpc/cases/04_nvshmem/putget.cu makes for NVSHMEM, and for the
// same reason: a workload that prints OK regardless is how case 12 passed for
// three weeks with nothing in it.
//
// Exit codes: 0 OK, 2 a CUDA/NCCL call failed, 3 data did not arrive, 4 this
// communicator has no GIN (the case skips on 4 and only on 4).

#include <mpi.h>

#include <cuda_runtime.h>
#include <nccl.h>
#include <nccl_device.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CUDA(call)                                                                        \
    do {                                                                                  \
        cudaError_t e_ = (call);                                                          \
        if (e_ != cudaSuccess) {                                                          \
            std::fprintf(stderr, "gin_putring: %s: %s\n", #call, cudaGetErrorString(e_)); \
            MPI_Abort(MPI_COMM_WORLD, 2);                                                 \
        }                                                                                 \
    } while (0)

#define NCCL(call)                                                                          \
    do {                                                                                    \
        ncclResult_t r_ = (call);                                                           \
        if (r_ != ncclSuccess) {                                                            \
            std::fprintf(stderr, "gin_putring: %s: %s\n", #call, ncclGetErrorString(r_));   \
            MPI_Abort(MPI_COMM_WORLD, 2);                                                   \
        }                                                                                   \
    } while (0)

// One word per (rank, iteration, index), different in every coordinate, so a
// word from the wrong sender, the wrong iteration or the wrong offset all fail.
__host__ __device__ inline std::uint32_t pattern(int rank, int iter, std::size_t i) {
    std::uint32_t x = static_cast<std::uint32_t>(rank) * 0x9E3779B1u ^
                      static_cast<std::uint32_t>(iter) * 0x85EBCA77u ^
                      static_cast<std::uint32_t>(i) * 0xC2B2AE3Du;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

__global__ void fill(std::uint32_t* buf, std::size_t words, int rank, int iter) {
    for (std::size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < words;
         i += static_cast<std::size_t>(gridDim.x) * blockDim.x)
        buf[i] = pattern(rank, iter, i);
}

__global__ void check(const std::uint32_t* buf, std::size_t words, int src, int iter,
                      unsigned long long* bad) {
    for (std::size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < words;
         i += static_cast<std::size_t>(gridDim.x) * blockDim.x)
        if (buf[i] != pattern(src, iter, i))
            atomicAdd(bad, 1ull);
}

// `expect` is cumulative — every put the left neighbour has made to us since the
// device communicator was created — so there is no window in which a put that
// lands before this kernel starts could be counted against the wrong iteration.
__global__ void put_ring(ncclWindow_t sendw, ncclWindow_t recvw, std::size_t chunk, int nput,
                         std::uint64_t expect, ncclDevComm dc) {
    ncclGin   gin{dc, 0};
    const int peer = (dc.rank + 1) % dc.nRanks;
    for (int p = blockIdx.x * blockDim.x + threadIdx.x; p < nput; p += gridDim.x * blockDim.x)
        gin.put(ncclTeamWorld(dc), peer, recvw, static_cast<std::size_t>(p) * chunk, sendw,
                static_cast<std::size_t>(p) * chunk, chunk, ncclGin_SignalInc{0});
    if (blockIdx.x == 0)
        gin.waitSignal(ncclCoopCta(), 0, expect);
    gin.flush(ncclCoopCta());
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 1, lrank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm local;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank, MPI_INFO_NULL, &local);
    MPI_Comm_rank(local, &lrank);

    const std::size_t chunk = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 128;
    const int         nput = argc > 2 ? std::atoi(argv[2]) : 1024;
    const int         iters = argc > 3 ? std::atoi(argv[3]) : 20;
    if (chunk == 0 || chunk % 4 != 0 || nput <= 0 || iters <= 0) {
        if (rank == 0)
            std::fprintf(stderr, "usage: gin_putring [chunk_bytes%%4==0] [puts_per_iter] [iters]\n");
        MPI_Abort(MPI_COMM_WORLD, 2);
    }
    const std::size_t bytes = chunk * static_cast<std::size_t>(nput);
    const std::size_t words = bytes / 4;

    int ndev = 0;
    CUDA(cudaGetDeviceCount(&ndev));
    CUDA(cudaSetDevice(lrank % ndev));

    ncclUniqueId id;
    if (rank == 0)
        NCCL(ncclGetUniqueId(&id));
    MPI_Bcast(&id, sizeof id, MPI_BYTE, 0, MPI_COMM_WORLD);
    ncclComm_t comm;
    NCCL(ncclCommInitRank(&comm, size, id, rank));

    // ncclCommQueryProperties arrived in 2.29. Before that the only way to ask
    // is to create the device communicator and see (below), so the version gate
    // is on the query, not on the workload: du04 ships 2.28.9 and MN5 2.29.3,
    // and this case has to be the same workload on both.
    const char* gin_name = "unqueried (NCCL < 2.29)";
#if defined(NCCL_VERSION_CODE) && NCCL_VERSION_CODE >= 22900
    ncclCommProperties_t props = NCCL_COMM_PROPERTIES_INITIALIZER;
    NCCL(ncclCommQueryProperties(comm, &props));
    gin_name = props.ginType == NCCL_GIN_TYPE_GDAKI   ? "GDAKI"
               : props.ginType == NCCL_GIN_TYPE_PROXY ? "PROXY"
                                                      : "NONE";
    if (!props.deviceApiSupport || props.ginType == NCCL_GIN_TYPE_NONE) {
        std::fprintf(stderr, "gin_putring: rank %d: no GIN on this communicator "
                             "(deviceApiSupport=%d ginType=%d)\n",
                     rank, props.deviceApiSupport, static_cast<int>(props.ginType));
        MPI_Abort(MPI_COMM_WORLD, 4);
    }
#endif

    void *sendp = nullptr, *recvp = nullptr;
    NCCL(ncclMemAlloc(&sendp, bytes));
    NCCL(ncclMemAlloc(&recvp, bytes));
    ncclWindow_t sendw, recvw;
    NCCL(ncclCommWindowRegister(comm, sendp, bytes, &sendw, NCCL_WIN_COLL_SYMMETRIC));
    NCCL(ncclCommWindowRegister(comm, recvp, bytes, &recvw, NCCL_WIN_COLL_SYMMETRIC));

    // 2.28 has no initializer macro and no size/magic/version fields in this
    // struct; 2.29 has both and rejects a requirements block whose header is not
    // filled in. The GIN fields are spelled the same in both, so one workload
    // serves du04 (2.28.9) and MN5 (2.29.3).
    ncclDevCommRequirements reqs = {};
#if defined(NCCL_VERSION_CODE) && NCCL_VERSION_CODE >= 22900
    reqs = NCCL_DEV_COMM_REQUIREMENTS_INITIALIZER;
#endif
    reqs.ginForceEnable = true;
    reqs.ginContextCount = 1;
    reqs.ginSignalCount = 1;

    // Not NCCL(): on a communicator without GIN this is where a pre-2.29 NCCL
    // finds out, and "this host cannot run the case" has to stay distinguishable
    // from "the case failed" — exit 4 is the skip, and only this and the
    // property query above produce it.
    ncclDevComm  dc;
    ncclResult_t dcr = ncclDevCommCreate(comm, &reqs, &dc);
    if (dcr != ncclSuccess) {
        std::fprintf(stderr,
                     "gin_putring: rank %d: no device communicator with GIN here (%s)\n", rank,
                     ncclGetErrorString(dcr));
        MPI_Abort(MPI_COMM_WORLD, 4);
    }

    unsigned long long* d_bad = nullptr;
    CUDA(cudaMalloc(&d_bad, sizeof *d_bad));
    CUDA(cudaMemset(d_bad, 0, sizeof *d_bad));
    CUDA(cudaMemset(recvp, 0, bytes));

    const int left = (rank + size - 1) % size;
    const int threads = 256;
    const int blocks = (nput + threads - 1) / threads < 16 ? (nput + threads - 1) / threads : 16;
    for (int it = 0; it < iters; ++it) {
        fill<<<64, 256>>>(static_cast<std::uint32_t*>(sendp), words, rank, it);
        CUDA(cudaGetLastError());
        CUDA(cudaDeviceSynchronize());
        // Nobody puts iteration `it` until everybody has checked iteration it-1.
        MPI_Barrier(MPI_COMM_WORLD);
        put_ring<<<blocks, threads>>>(sendw, recvw, chunk, nput,
                                      static_cast<std::uint64_t>(it + 1) * nput, dc);
        CUDA(cudaGetLastError());
        CUDA(cudaDeviceSynchronize());
        check<<<64, 256>>>(static_cast<const std::uint32_t*>(recvp), words, left, it, d_bad);
        CUDA(cudaGetLastError());
        CUDA(cudaDeviceSynchronize());
    }

    unsigned long long bad = 0, bad_all = 0;
    CUDA(cudaMemcpy(&bad, d_bad, sizeof bad, cudaMemcpyDeviceToHost));
    MPI_Allreduce(&bad, &bad_all, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (bad) {
        std::fprintf(stderr, "gin_putring: rank %d: %llu of %zu words wrong over %d iters\n", rank,
                     bad, words * iters, iters);
    }
    if (bad_all)
        MPI_Abort(MPI_COMM_WORLD, 3);
    if (rank == 0)
        std::printf("gin put-ring OK: %d ranks, %d puts x %zu B, %d iters, every byte arrived "
                    "(gin=%s)\n",
                    size, nput, chunk, iters, gin_name);

    NCCL(ncclDevCommDestroy(comm, &dc));
    NCCL(ncclCommWindowDeregister(comm, sendw));
    NCCL(ncclCommWindowDeregister(comm, recvw));
    NCCL(ncclMemFree(sendp));
    NCCL(ncclMemFree(recvp));
    CUDA(cudaFree(d_bad));
    NCCL(ncclCommFinalize(comm));
    NCCL(ncclCommDestroy(comm));
    MPI_Comm_free(&local);
    MPI_Finalize();
    return 0;
}
