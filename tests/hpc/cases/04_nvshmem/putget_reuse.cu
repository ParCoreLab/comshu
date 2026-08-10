// NVSHMEM put-ring that REUSES a symmetric address, on purpose.
//
// Exists to exercise one path nothing else in the suite reaches: a handle whose
// address is freed and reallocated, so the same interned handle names two
// different buffers. `mem_owner` merged those until lifetimes landed, charging
// one buffer's accesses to the other with no column saying so — and the branch
// that now separates them had no capture to run against.
//
// The shape is putget.cu's, with one variable changed: two rounds, with a free
// between them. NVSHMEM sub-allocates from its own mspace (a dlmalloc
// derivative over the symmetric heap), and a freed chunk of the same size is
// the obvious one to hand back, so round 2 is expected to land on round 1's
// address. That is an expectation about an allocator, not a guarantee, so the
// program CHECKS it and says so rather than assuming the test did what it
// claims — a run where the address is not reused exercises nothing, and should
// say that out loud instead of passing quietly.
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include <mpi.h>
#include <nvshmem.h>
#include <nvshmemx.h>

__global__ void put_to_next(int* dst, const int* src, int n, int peer) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        nvshmem_int_p(&dst[i], src[i], peer);
}

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
    cudaSetDevice(local);

    MPI_Comm             comm = MPI_COMM_WORLD;
    nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
    attr.mpi_comm = &comm;
    nvshmemx_init_attr(NVSHMEMX_INIT_WITH_MPI_COMM, &attr);

    const int mype = nvshmem_my_pe();
    const int npes = nvshmem_n_pes();
    const int peer = (mype + 1) % npes;

    const int n = argc > 1 ? atoi(argv[1]) : 1024;
    const int iters = argc > 2 ? atoi(argv[2]) : 10;

    int*        src = static_cast<int*>(nvshmem_malloc(n * sizeof(int)));
    void*       first = nullptr;
    dim3        block(256), grid((n + 255) / 256);

    // Two rounds; the buffer is freed between them.
    for (int round = 0; round < 2; ++round) {
        int* dst = static_cast<int*>(nvshmem_malloc(n * sizeof(int)));
        if (round == 0)
            first = dst;
        cudaMemset(src, round + 1, n * sizeof(int));

        for (int it = 0; it < iters; ++it) {
            put_to_next<<<grid, block>>>(dst, src, n, peer);
            nvshmemx_barrier_all_on_stream(0);
        }
        cudaDeviceSynchronize();
        nvshmem_barrier_all();

        if (mype == 0)
            printf("round %d: dst=%p\n", round, (void*)dst);

        // The free that makes the address available again. nvshmem_barrier_all
        // first so no peer is still writing into it — a peer store landing
        // after the free would be a genuine use-after-free in the workload, not
        // a tracing question, and would make the capture unreadable as evidence.
        nvshmem_barrier_all();
        nvshmem_free(dst);

        if (round == 1) {
            const bool reused = (dst == first);
            if (mype == 0)
                printf("nvshmem put-reuse %s: %d PEs, %d ints, %d iters, address %s\n",
                       reused ? "OK" : "INCONCLUSIVE", npes, n, iters,
                       reused ? "REUSED across rounds" : "NOT reused — nothing exercised");
        }
    }

    nvshmem_free(src);
    nvshmem_finalize();
    MPI_Finalize();
    return 0;
}
