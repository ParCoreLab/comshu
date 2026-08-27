// The doorbell hijack's overhead, as a function of how much work the
// application does between doorbells.
//
// WHY THIS EXISTS. Stages 1 and 2 of the IBGDA data-plane hijack were measured
// at +68% and +86% wall — on `04_nvshmem/putget.cu`, which is a put-ring with
// NO COMPUTE. Every microsecond of that workload is either a doorbell or a
// barrier, so it is close to the worst case a doorbell interposer can meet, and
// PLAN-20260807-1746's close says so explicitly: "they were measured on a
// doorbell-bound loopback microbenchmark that is close to the worst case. The
// honest next step is a Stage-2 measurement on a workload with real compute
// (where the doorbell rate is a fraction of the work) before committing to a
// persistent green-context kernel."
//
// This is that workload. It is the same put-ring with one knob added — FLOPs per
// iteration — so the two points are comparable by construction: at
// `compute_steps = 0` it IS the old benchmark, and everything above that walks
// the ratio out towards what a real application looks like. One number cannot
// answer "is +86% representative"; a curve can.
//
//   stencil_putget <elems> <iters> <compute_steps>
//
// IT VERIFIES ITS OWN OUTPUT, and that is not optional here. The hijack sits IN
// THE DATA PATH — it owns the doorbell, so a dropped or reordered forward
// corrupts a transfer rather than emptying a table. A timing harness that did
// not check the bytes could report a beautiful speedup for a run that moved the
// wrong data. (The stock putget.cu prints OK on completion without checking what
// it received, which is why Stage 1 needed a verifying variant of its own.)

#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include <mpi.h>
#include <nvshmem.h>
#include <nvshmemx.h>

// Local compute: `steps` fused multiply-adds per element, touching only local
// memory. Deliberately arithmetic rather than bandwidth-bound — the point is to
// occupy the GPU for a controllable time WITHOUT generating memory traffic that
// would itself perturb the measurement.
//
// `work` is read back into `src` at the end so nvcc cannot fold the loop away;
// the value is bounded and the verification below accounts for it.
__global__ void compute(float* scratch, int n, int steps) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    float acc = scratch[i];
    for (int s = 0; s < steps; ++s)
        acc = fmaf(acc, 1.0000001f, 1e-7f);
    scratch[i] = acc;
}

// The communication, unchanged from 04_nvshmem/putget.cu: one device-side
// nvshmem_int_p per element to the next PE. Under IBGDA each of these becomes a
// GPU-built WQE and a doorbell rung from the device — the traffic the hijack
// exists to see.
__global__ void put_to_next(int* dst, const int* src, int n, int peer) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
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
    const int prev = (mype + npes - 1) % npes;

    const int n = argc > 1 ? atoi(argv[1]) : 1024;
    const int iters = argc > 2 ? atoi(argv[2]) : 20;
    const int steps = argc > 3 ? atoi(argv[3]) : 0;

    int*   dst = static_cast<int*>(nvshmem_malloc(n * sizeof(int)));
    int*   src = static_cast<int*>(nvshmem_malloc(n * sizeof(int)));
    float* scratch = nullptr;
    cudaMalloc(&scratch, n * sizeof(float));
    cudaMemset(scratch, 0, n * sizeof(float));

    // EVERY PE SENDS A VALUE ONLY IT COULD HAVE SENT. `mype + 1` means the
    // receiver can name its sender, so a forward that went to the wrong queue
    // pair is caught rather than averaging out — which a buffer of all-ones
    // (what putget.cu memsets) cannot do.
    int* host = static_cast<int*>(malloc(n * sizeof(int)));
    for (int i = 0; i < n; ++i)
        host[i] = mype + 1;
    cudaMemcpy(src, host, n * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemset(dst, 0, n * sizeof(int));

    dim3 block(256), grid((n + 255) / 256);

    // Warm up both kernels and the NVSHMEM connection setup, so the timed loop
    // measures steady-state doorbell traffic and not first-touch page faults,
    // JIT, or queue-pair creation.
    compute<<<grid, block>>>(scratch, n, steps);
    put_to_next<<<grid, block>>>(dst, src, n, peer);
    nvshmemx_barrier_all_on_stream(0);
    cudaDeviceSynchronize();
    nvshmem_barrier_all();

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0);
    cudaEventCreate(&t1);
    MPI_Barrier(MPI_COMM_WORLD);
    cudaEventRecord(t0);
    for (int it = 0; it < iters; ++it) {
        compute<<<grid, block>>>(scratch, n, steps);
        put_to_next<<<grid, block>>>(dst, src, n, peer);
        nvshmemx_barrier_all_on_stream(0);
    }
    cudaEventRecord(t1);
    cudaDeviceSynchronize();
    nvshmem_barrier_all();
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, t0, t1);

    // What arrived must be what the PREVIOUS PE sent, in every element.
    cudaMemcpy(host, dst, n * sizeof(int), cudaMemcpyDeviceToHost);
    int bad = 0;
    for (int i = 0; i < n; ++i)
        if (host[i] != prev + 1)
            ++bad;

    // One line per PE, and the fields are named so the harness can grep rather
    // than count columns.
    printf("stencil_putget pe=%d npes=%d elems=%d iters=%d steps=%d ms=%.3f bad=%d %s\n", mype,
           npes, n, iters, steps, ms, bad, bad ? "VERIFY FAIL" : "VERIFY OK");
    fflush(stdout);

    free(host);
    cudaFree(scratch);
    nvshmem_free(dst);
    nvshmem_free(src);
    nvshmem_finalize();
    MPI_Finalize();
    return bad ? 1 : 0;
}
