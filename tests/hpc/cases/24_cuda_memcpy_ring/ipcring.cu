// Ad-hoc probe: does a peer copy become a rank->rank edge in comm_edges?
//
// Case 23 runs ONE process driving four GPUs, so every copy edge it produces is
// rank 0 -> rank 0 — correct, and silent about the resolution chain
// (_node -> _device_key -> _device_owner) that turns a device ordinal into an
// owning rank. tests/comm_edges_rule.sh exercises that chain on a synthetic
// five-rank two-node fixture; this is the hardware confirmation.
//
// Four MPI ranks, one GPU each, a ring: rank r opens rank (r+1)%n's allocation
// with CUDA IPC and cudaMemcpyPeer's into it. Expected matrix: r -> r+1 for
// every r, `iters` copies of `bytes` on each edge, and nothing else.
#include <cstdio>
#include <cstdlib>
#include <mpi.h>
#include <cuda_runtime.h>

#define CK(x) do { cudaError_t e=(x); if(e!=cudaSuccess){ \
    std::fprintf(stderr,"ipcring rank %d: %s:%d %s: %s\n",rank,__FILE__,__LINE__,#x, \
                 cudaGetErrorString(e)); MPI_Abort(MPI_COMM_WORLD,2);} } while(0)

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, n = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &n);
    const int    iters = argc > 1 ? std::atoi(argv[1]) : 100;
    const size_t bytes = argc > 2 ? (size_t)std::atoll(argv[2]) : (1u << 20);

    // Every rank sees every GPU (no CUDA_VISIBLE_DEVICES pinning): cudaMemcpyPeer
    // needs the PEER's ordinal to be meaningful in THIS process, so the ordinals
    // have to be the node-wide ones. That is also the configuration in which
    // _device_owner has real work to do -- four ranks each claiming one of four
    // visible devices.
    CK(cudaSetDevice(rank % n));
    void* mine = nullptr;
    CK(cudaMalloc(&mine, bytes));
    CK(cudaMemset(mine, rank + 1, bytes));

    cudaIpcMemHandle_t h{}, theirs{};
    CK(cudaIpcGetMemHandle(&h, mine));
    const int next = (rank + 1) % n, prev = (rank + n - 1) % n;
    MPI_Sendrecv(&h, sizeof h, MPI_BYTE, prev, 0, &theirs, sizeof theirs, MPI_BYTE, next, 0,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    void* peer = nullptr;
    CK(cudaIpcOpenMemHandle(&peer, theirs, cudaIpcMemLazyEnablePeerAccess));
    MPI_Barrier(MPI_COMM_WORLD);

    for (int i = 0; i < iters; ++i)
        CK(cudaMemcpyPeer(peer, next, mine, rank % n, bytes));
    CK(cudaDeviceSynchronize());
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0)
        std::printf("ipcring: %d ranks, %d iters of %zu B, ring r -> r+1\n", n, iters, bytes);
    CK(cudaIpcCloseMemHandle(peer));
    MPI_Finalize();
    return 0;
}
