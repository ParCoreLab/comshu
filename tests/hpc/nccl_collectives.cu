// NCCL API coverage workload — the one that exercises the host-API provider.
//
// tests/hpc/nccl_allreduce.cu drives exactly three NCCL entry points
// (ncclCommInitRank, ncclAllReduce, ncclCommDestroy), which is right for what
// it is for: cases 02 and 05 use it to make NCCL's TRANSPORT work, and the
// collective it picks is incidental to that. It leaves half the nccl provider's
// record types unexercised, and an untested record type is a table nobody has
// ever seen a row in.
//
// This one is the other shape: the collectives are small and the point is which
// DOORS get opened. Every record the provider can emit gets produced here —
// nccl_comm, nccl_collective (four ops, so the CollOp discriminator is checked
// against something other than a single value), nccl_p2p, nccl_group, and the
// EntityClose at teardown.
//
// GROUPED SEND/RECV IS THE PART NOTHING ELSE COVERS. A ring exchange inside
// ncclGroupStart/End is how every real alltoall and every pipeline-parallel
// stage is written, and it is the only NCCL call that names a peer — so it is
// the only one an exact comm edge follows from without modelling how a
// collective decomposes internally. The suite had no NCCL point-to-point at all
// before this.
//
// Deliberately small messages and few iterations: this case is about API
// coverage, not bandwidth. The volume assertions live in cases 02 and 05.
#include <cstdio>
#include <cstdlib>

#include <cuda_runtime.h>
#include <mpi.h>
#include <nccl.h>

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

    const size_t N = argc > 1 ? strtoul(argv[1], nullptr, 10) : 65536;  // floats
    const int    iters = argc > 2 ? atoi(argv[2]) : 2;

    // recv is sized for the all-gather, which produces `size` slices of N.
    float* send = nullptr;
    float* recv = nullptr;
    CK(cudaMalloc(&send, N * sizeof(float)));
    CK(cudaMalloc(&recv, N * sizeof(float) * (size_t)size));
    CK(cudaMemset(send, 1, N * sizeof(float)));
    CK(cudaMemset(recv, 0, N * sizeof(float) * (size_t)size));
    cudaStream_t s;
    CK(cudaStreamCreate(&s));

    for (int i = 0; i < iters; ++i) {
        // Four distinct CollOps. AllGather's count is the SEND count while
        // ReduceScatter's is the RECV count — each is the per-rank slice — which
        // is exactly the asymmetry the record keeps by storing what the API was
        // given rather than normalising the two into one "size".
        NK(ncclAllReduce(send, recv, N, ncclFloat, ncclSum, comm, s));
        NK(ncclBroadcast(send, recv, N, ncclFloat, /*root*/ 0, comm, s));
        NK(ncclAllGather(send, recv, N / (size_t)size, ncclFloat, comm, s));
        NK(ncclReduceScatter(send, recv, N / (size_t)size, ncclFloat, ncclSum, comm, s));

        // The ring exchange. Fused into ONE launch by the group, which is the
        // fact nccl_group exists to record: without it these 2 calls per rank
        // read as 2 separate transfers instead of one operation.
        const int next = (rank + 1) % size;
        const int prev = (rank - 1 + size) % size;
        NK(ncclGroupStart());
        NK(ncclSend(send, N, ncclFloat, next, comm, s));
        NK(ncclRecv(recv, N, ncclFloat, prev, comm, s));
        NK(ncclGroupEnd());
    }
    CK(cudaStreamSynchronize(s));

    if (rank == 0)
        printf("nccl_collectives OK: %d ranks, %zu floats, %d iters "
               "(4 collectives + 1 grouped ring exchange per iter)\n",
               size, N, iters);

    ncclCommDestroy(comm);
    CK(cudaFree(send));
    CK(cudaFree(recv));
    MPI_Finalize();
    return 0;
}
