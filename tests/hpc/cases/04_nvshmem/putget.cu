// NVSHMEM put-ring — a minimal PGAS workload for exercising the pcomm gpumem
// (NVBit) provider. Each PE writes its symmetric buffer into the next PE's
// symmetric buffer with device-side nvshmem_int_p; on one node those stores go
// over NVLink to peer GPU memory, which is exactly the GPU-initiated peer
// access Snoopie/gpumem instruments. The symmetric-heap allocation gives the
// provider its MemObject entities to attribute against.
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include <mpi.h>
#include <nvshmem.h>
#include <nvshmemx.h>

__global__ void put_to_next(int* dst, const int* src, int n, int peer) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) nvshmem_int_p(&dst[i], src[i], peer);
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int local = 0;
    for (const char* v : {"OMPI_COMM_WORLD_LOCAL_RANK", "SLURM_LOCALID"})
        if (const char* e = getenv(v)) { local = atoi(e); break; }
    cudaSetDevice(local);

    MPI_Comm comm = MPI_COMM_WORLD;
    nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
    attr.mpi_comm = &comm;
    nvshmemx_init_attr(NVSHMEMX_INIT_WITH_MPI_COMM, &attr);

    int mype = nvshmem_my_pe();
    int npes = nvshmem_n_pes();
    int peer = (mype + 1) % npes;

    int n = argc > 1 ? atoi(argv[1]) : (1 << 20);      // ints
    int iters = argc > 2 ? atoi(argv[2]) : 10;
    int* dst = static_cast<int*>(nvshmem_malloc(n * sizeof(int)));
    int* src = static_cast<int*>(nvshmem_malloc(n * sizeof(int)));
    cudaMemset(src, 1, n * sizeof(int));

    dim3 block(256), grid((n + 255) / 256);
    for (int it = 0; it < iters; ++it) {
        put_to_next<<<grid, block>>>(dst, src, n, peer);
        nvshmemx_barrier_all_on_stream(0);
    }
    cudaDeviceSynchronize();
    nvshmem_barrier_all();

    if (mype == 0)
        printf("nvshmem put-ring OK: %d PEs, %d ints, %d iters\n", npes, n, iters);

    nvshmem_free(dst);
    nvshmem_free(src);
    nvshmem_finalize();
    MPI_Finalize();
    return 0;
}
