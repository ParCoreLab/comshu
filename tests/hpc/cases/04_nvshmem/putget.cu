// NVSHMEM put-ring — a minimal PGAS workload for exercising the pcomm gpumem
// (NVBit) provider. Each PE writes its symmetric buffer into the next PE's
// symmetric buffer with device-side nvshmem_int_p; on one node those stores go
// over NVLink to peer GPU memory, which is exactly the GPU-initiated peer
// access Snoopie/gpumem instruments. The symmetric-heap allocation gives the
// provider its MemObject entities to attribute against.
//
// It CHECKS what it claims. Until 2026-09-15 it checked no CUDA call and printed
// "OK" unconditionally — and on MN5 the binary cases 09-12 and 25 share held
// sm_80 code only, no PTX. On an H100 the kernel never launched, NVSHMEM's host
// barriers still ran, and every run said "put-ring OK" having put nothing. Case
// 11's data_plane_puts_arrived is what noticed: 0 of 2,048 puts per rank, with
// the barrier traffic around them exactly as on the A100. So every launch is
// checked, the destination starts cleared, and each PE reads back what arrived.
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include <mpi.h>
#include <nvshmem.h>
#include <nvshmemx.h>
#include <vector>

// A failure on any PE ends the job non-zero, so the harness cannot mistake it for
// a run that finished.
static void check(cudaError_t e, const char* what) {
    if (e == cudaSuccess)
        return;
    std::fprintf(stderr, "putget: %s failed: %s\n", what, cudaGetErrorString(e));
    MPI_Abort(MPI_COMM_WORLD, 2);
}

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
    check(cudaSetDevice(local), "cudaSetDevice");

    MPI_Comm             comm = MPI_COMM_WORLD;
    nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
    attr.mpi_comm = &comm;
    nvshmemx_init_attr(NVSHMEMX_INIT_WITH_MPI_COMM, &attr);

    int mype = nvshmem_my_pe();
    int npes = nvshmem_n_pes();
    int peer = (mype + 1) % npes;

    int          n = argc > 1 ? atoi(argv[1]) : (1 << 20);  // ints
    int          iters = argc > 2 ? atoi(argv[2]) : 10;
    const size_t bytes = static_cast<size_t>(n) * sizeof(int);
    int*         dst = static_cast<int*>(nvshmem_malloc(bytes));
    int*         src = static_cast<int*>(nvshmem_malloc(bytes));
    // dst starts cleared, and nobody puts until everyone has cleared: a run
    // whose puts never happen must not find the right bytes there by accident.
    check(cudaMemset(dst, 0, bytes), "cudaMemset(dst)");
    check(cudaMemset(src, 1, bytes), "cudaMemset(src)");
    check(cudaDeviceSynchronize(), "cudaDeviceSynchronize (setup)");
    nvshmem_barrier_all();

    dim3 block(256), grid((n + 255) / 256);
    for (int it = 0; it < iters; ++it) {
        put_to_next<<<grid, block>>>(dst, src, n, peer);
        check(cudaGetLastError(), "put_to_next launch");
        nvshmemx_barrier_all_on_stream(0);
    }
    check(cudaDeviceSynchronize(), "cudaDeviceSynchronize (puts)");
    nvshmem_barrier_all();

    // Every int of dst came from the previous PE's src, which cudaMemset set to
    // 0x01 in each byte.
    if (iters > 0) {
        std::vector<int> got(static_cast<size_t>(n));
        check(cudaMemcpy(got.data(), dst, bytes, cudaMemcpyDeviceToHost), "cudaMemcpy(dst)");
        long missing = 0;
        for (int v : got)
            missing += v != 0x01010101;
        if (missing) {
            std::fprintf(stderr, "putget: PE %d: %ld of %d ints did not arrive\n", mype, missing,
                         n);
            MPI_Abort(MPI_COMM_WORLD, 3);
        }
    }
    nvshmem_barrier_all();

    if (mype == 0)
        printf("nvshmem put-ring OK: %d PEs, %d ints, %d iters, every int arrived\n", npes, n,
               iters);

    nvshmem_free(dst);
    nvshmem_free(src);
    nvshmem_finalize();
    MPI_Finalize();
    return 0;
}
