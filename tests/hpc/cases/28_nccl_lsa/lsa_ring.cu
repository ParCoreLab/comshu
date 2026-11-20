// lsa_ring — a store ring through NCCL's symmetric windows, over NVLink.
//
//   mpirun -np <P> lsa_ring [window_bytes=131072] [iters=10] [blocking|nonblocking]
//
// NCCL 2.27+ maps every local rank's window into ONE flat address range: peer
// r's copy of a window sits at lsaFlatBase + r * stride + offset, and an offset
// names the same byte on every rank (ncclGetLsaPointer, nccl_device/core.h).
// That is the NVSHMEM symmetric-heap shape, and this workload exists to find
// out whether comshu places such stores in the OWNER's buffer the way it places
// NVSHMEM's.
//
// No NCCL collective and no device communicator: the kernel computes the peer
// pointer itself and issues plain 4-byte stores, one word per thread per pass,
// so the gpumem provider sees exactly window_bytes / 4 peer stores per rank per
// iteration, and every one targets the right neighbour's window at offset 4*i.
//
// Every iteration each rank writes a pattern unique to (rank, iteration, word)
// into its right neighbour's window and then checks its own window against its
// left neighbour's pattern. A missing, stale or misplaced word fails the job
// non-zero, so "OK" is a claim — the same promise gin_putring makes.
//
// `nonblocking` (case 29) builds the communicator with config.blocking = 0 and
// registers the window INSIDE an ncclGroupStart/End, so every setup call
// returns ncclInProgress and the window handle is written by NCCL's own thread
// after ncclGroupEnd has returned. Each step then polls ncclCommGetAsyncError
// until the communicator is ready, as NCCL requires before the handle may be
// used. The ring itself is the same, so the same stores must land in the same
// places: what changes is only WHEN the capture can learn where they land.
//
// Exit codes: 0 OK, 2 a CUDA/NCCL call failed, 3 data did not arrive, 4 this
// communicator has no symmetric-memory support (the case skips on 4 only).

#include <mpi.h>

#include <cuda_runtime.h>
#include <nccl.h>
#include <nccl_device.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CUDA(call)                                                                      \
    do {                                                                                \
        cudaError_t e_ = (call);                                                        \
        if (e_ != cudaSuccess) {                                                        \
            std::fprintf(stderr, "lsa_ring: %s: %s\n", #call, cudaGetErrorString(e_));  \
            MPI_Abort(MPI_COMM_WORLD, 2);                                               \
        }                                                                               \
    } while (0)

#define NCCL(call)                                                                      \
    do {                                                                                \
        ncclResult_t r_ = (call);                                                       \
        if (r_ != ncclSuccess) {                                                        \
            std::fprintf(stderr, "lsa_ring: %s: %s\n", #call, ncclGetErrorString(r_));  \
            MPI_Abort(MPI_COMM_WORLD, 2);                                               \
        }                                                                               \
    } while (0)

// A non-blocking communicator answers issued work with ncclInProgress and
// finishes it on NCCL's thread; ready is the first ncclCommGetAsyncError that
// says ncclSuccess. On a blocking one this returns at once.
static void wait_ready(ncclComm_t comm, ncclResult_t issued_as, const char* what) {
    if (issued_as != ncclSuccess && issued_as != ncclInProgress) {
        std::fprintf(stderr, "lsa_ring: %s: %s\n", what, ncclGetErrorString(issued_as));
        MPI_Abort(MPI_COMM_WORLD, 2);
    }
    ncclResult_t state = ncclInProgress;
    do {
        NCCL(ncclCommGetAsyncError(comm, &state));
    } while (state == ncclInProgress);
    if (state != ncclSuccess) {
        std::fprintf(stderr, "lsa_ring: %s: async %s\n", what, ncclGetErrorString(state));
        MPI_Abort(MPI_COMM_WORLD, 2);
    }
}

__host__ __device__ inline std::uint32_t pattern(int rank, int iter, std::size_t i) {
    std::uint32_t x = static_cast<std::uint32_t>(rank) * 0x9E3779B1u ^
                      static_cast<std::uint32_t>(iter) * 0x85EBCA77u ^
                      static_cast<std::uint32_t>(i) * 0xC2B2AE3Du;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

// The only peer access in the program. `volatile` keeps every store a single
// 4-byte store, so the provider's count is words, not whatever the compiler
// chose to vectorise into.
__global__ void put_ring(ncclWindow_t w, std::size_t words, int lsa_peer, int rank, int iter) {
    for (std::size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < words;
         i += static_cast<std::size_t>(gridDim.x) * blockDim.x) {
        volatile std::uint32_t* p =
            static_cast<volatile std::uint32_t*>(ncclGetLsaPointer(w, i * 4, lsa_peer));
        *p = pattern(rank, iter, i);
    }
}

__global__ void check(const std::uint32_t* buf, std::size_t words, int src, int iter,
                      unsigned long long* bad) {
    for (std::size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < words;
         i += static_cast<std::size_t>(gridDim.x) * blockDim.x)
        if (buf[i] != pattern(src, iter, i))
            atomicAdd(bad, 1ull);
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 1, lrank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    MPI_Comm local;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank, MPI_INFO_NULL, &local);
    MPI_Comm_rank(local, &lrank);

    const std::size_t bytes = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 131072;
    const int         iters = argc > 2 ? std::atoi(argv[2]) : 10;
    const bool        nonblocking = argc > 3 && std::strcmp(argv[3], "nonblocking") == 0;
    if (bytes == 0 || bytes % 4 != 0 || iters <= 0 ||
        (argc > 3 && !nonblocking && std::strcmp(argv[3], "blocking") != 0)) {
        if (rank == 0)
            std::fprintf(stderr,
                         "usage: lsa_ring [window_bytes%%4==0] [iters] [blocking|nonblocking]\n");
        MPI_Abort(MPI_COMM_WORLD, 2);
    }
    const std::size_t words = bytes / 4;

    int ndev = 0;
    CUDA(cudaGetDeviceCount(&ndev));
    CUDA(cudaSetDevice(lrank % ndev));

    ncclUniqueId id;
    if (rank == 0)
        NCCL(ncclGetUniqueId(&id));
    MPI_Bcast(&id, sizeof id, MPI_BYTE, 0, MPI_COMM_WORLD);
    ncclComm_t comm;
    if (nonblocking) {
        ncclConfig_t cfg = NCCL_CONFIG_INITIALIZER;
        cfg.blocking = 0;
        const ncclResult_t r = ncclCommInitRankConfig(&comm, size, id, rank, &cfg);
        wait_ready(comm, r, "init");  // two statements: comm is written by the call
    } else {
        NCCL(ncclCommInitRank(&comm, size, id, rank));
    }

    // The ring is over the LSA team, whose ranks are the ones a flat-range
    // pointer can reach. On one node that is every rank, and the job runs on
    // one node; a team smaller than the world means the topology is not the
    // one this case claims, which is a skip, not a pass.
    ncclTeam_t lsa = ncclTeamLsa(comm);
    if (lsa.nRanks != size) {
        std::fprintf(stderr, "lsa_ring: rank %d: LSA team has %d of %d ranks — no symmetric "
                             "memory across this communicator\n", rank, lsa.nRanks, size);
        MPI_Abort(MPI_COMM_WORLD, 4);
    }
    const int lsa_peer = (lsa.rank + 1) % lsa.nRanks;
    const int left = (rank + size - 1) % size;

    void* winp = nullptr;
    NCCL(ncclMemAlloc(&winp, bytes));
    ncclWindow_t w = nullptr;
    ncclResult_t wr;
    if (nonblocking) {
        // Inside a group: the call returns with w still null, and NCCL's thread
        // writes it after ncclGroupEnd returns ncclInProgress.
        NCCL(ncclGroupStart());
        wr = ncclCommWindowRegister(comm, winp, bytes, &w, NCCL_WIN_COLL_SYMMETRIC);
        const ncclResult_t ge = ncclGroupEnd();
        if (wr == ncclSuccess) {
            wait_ready(comm, ge, "window register");
            if (!w)
                wr = ncclInvalidUsage;  // ready, and NCCL registered nothing
        }
    } else {
        wr = ncclCommWindowRegister(comm, winp, bytes, &w, NCCL_WIN_COLL_SYMMETRIC);
    }
    if (wr != ncclSuccess) {
        std::fprintf(stderr, "lsa_ring: rank %d: no symmetric window here (%s)\n", rank,
                     ncclGetErrorString(wr));
        MPI_Abort(MPI_COMM_WORLD, 4);
    }

    unsigned long long* d_bad = nullptr;
    CUDA(cudaMalloc(&d_bad, sizeof *d_bad));
    CUDA(cudaMemset(d_bad, 0, sizeof *d_bad));
    CUDA(cudaMemset(winp, 0, bytes));
    CUDA(cudaDeviceSynchronize());

    for (int it = 0; it < iters; ++it) {
        // Nobody writes iteration `it` until everybody has checked it-1.
        MPI_Barrier(MPI_COMM_WORLD);
        put_ring<<<64, 256>>>(w, words, lsa_peer, rank, it);
        CUDA(cudaGetLastError());
        CUDA(cudaDeviceSynchronize());
        // Every rank's stores are complete before anyone reads.
        MPI_Barrier(MPI_COMM_WORLD);
        check<<<64, 256>>>(static_cast<const std::uint32_t*>(winp), words, left, it, d_bad);
        CUDA(cudaGetLastError());
        CUDA(cudaDeviceSynchronize());
    }

    unsigned long long bad = 0, bad_all = 0;
    CUDA(cudaMemcpy(&bad, d_bad, sizeof bad, cudaMemcpyDeviceToHost));
    MPI_Allreduce(&bad, &bad_all, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (bad)
        std::fprintf(stderr, "lsa_ring: rank %d: %llu of %zu words wrong over %d iters\n", rank,
                     bad, words * iters, iters);
    if (bad_all)
        MPI_Abort(MPI_COMM_WORLD, 3);
    if (rank == 0)
        std::printf("lsa ring OK: %d ranks, %zu B window, %d iters, %s, every word arrived\n",
                    size, bytes, iters, nonblocking ? "non-blocking" : "blocking");

    wait_ready(comm, ncclCommWindowDeregister(comm, w), "window deregister");
    NCCL(ncclMemFree(winp));
    CUDA(cudaFree(d_bad));
    wait_ready(comm, ncclCommFinalize(comm), "finalize");
    NCCL(ncclCommDestroy(comm));
    MPI_Comm_free(&local);
    MPI_Finalize();
    return 0;
}
