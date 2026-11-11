// Minimal NCCL all-reduce over MPI bootstrap, for exercising the comshu ibverbs
// provider on a real collective. Inter-node all-reduce drives NCCL's IB
// transport (ibv_reg_mr on GPU buffers, create_qp/modify_qp topology,
// ibv_post_send/poll_cq on the proxy threads).
#include <cstdio>
#include <cstdlib>
#include <mpi.h>
#include <nccl.h>
#include <cuda_runtime.h>

#define CK(x)                                                                                      \
    do {                                                                                           \
        cudaError_t e = (x);                                                                       \
        if (e != cudaSuccess) {                                                                    \
            fprintf(stderr, "[%d] cuda: %s\n", rank, cudaGetErrorString(e));                       \
            MPI_Abort(MPI_COMM_WORLD, 1);                                                          \
        }                                                                                          \
    } while (0)
#define NK(x)                                                                                      \
    do {                                                                                           \
        ncclResult_t e = (x);                                                                      \
        if (e != ncclSuccess) {                                                                    \
            fprintf(stderr, "[%d] nccl: %s\n", rank, ncclGetErrorString(e));                       \
            MPI_Abort(MPI_COMM_WORLD, 1);                                                          \
        }                                                                                          \
    } while (0)

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int local = 0;
    for (const char* v : {"OMPI_COMM_WORLD_LOCAL_RANK", "SLURM_LOCALID"})
        if (const char* e = getenv(v)) {
            local = atoi(e);
            break;
        }
    CK(cudaSetDevice(local));

    ncclUniqueId id;
    if (rank == 0)
        NK(ncclGetUniqueId(&id));
    MPI_Bcast(&id, sizeof(id), MPI_BYTE, 0, MPI_COMM_WORLD);

    ncclComm_t comm;
    NK(ncclCommInitRank(&comm, size, id, rank));

    size_t N = argc > 1 ? strtoul(argv[1], nullptr, 10) : (8UL << 20);  // floats
    int    iters = argc > 2 ? atoi(argv[2]) : 20;
    float* buf = nullptr;
    CK(cudaMalloc(&buf, N * sizeof(float)));
    CK(cudaMemset(buf, 1, N * sizeof(float)));
    cudaStream_t s;
    CK(cudaStreamCreate(&s));

    for (int i = 0; i < iters; ++i)
        NK(ncclAllReduce(buf, buf, N, ncclFloat, ncclSum, comm, s));
    CK(cudaStreamSynchronize(s));

    if (rank == 0)
        printf("allreduce OK: %d ranks, %zu floats (%zu MB), %d iters\n", size, N,
               N * sizeof(float) >> 20, iters);

    ncclCommDestroy(comm);
    CK(cudaFree(buf));
    MPI_Finalize();
    return 0;
}
