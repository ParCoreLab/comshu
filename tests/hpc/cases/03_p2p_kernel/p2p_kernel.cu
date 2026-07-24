// p2p_kernel — a minimal, deterministic intra-node peer read/write microkernel,
// the clean target for the gpumem (NVBit) provider. One process drives all GPUs;
// each GPU r runs a kernel that stores into the *next* GPU's buffer over NVLink
// (a peer store, run_device=r, mem_device=r+1) and reads its own buffer (a local
// load). Discrete kernels — not a persistent collective — so NVBit can drain its
// channel between launches (NCCL's persistent kernels deadlock under NVBit).
//
// Deterministic by construction: for N GPUs, ELEMS threads each, ITERS times,
// every GPU issues ITERS*ELEMS peer stores to its neighbor and ITERS*ELEMS local
// loads — a known golden the gpumem provider must reproduce.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cuda_runtime.h>

#define CK(x) do{ cudaError_t e=(x); if(e!=cudaSuccess){ \
  fprintf(stderr,"cuda %s:%d: %s\n",__FILE__,__LINE__,cudaGetErrorString(e)); exit(1);} }while(0)

// peer[] lives on the neighbor GPU; local[] on this one. The store to peer[i]
// crosses NVLink; the load of local[i] stays home.
__global__ void peer_rw(int* peer, const int* local, int n, int add) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) peer[i] = local[i] + add;
}

int main(int argc, char** argv) {
    int n     = argc > 1 ? atoi(argv[1]) : 4096;   // elements per GPU
    int iters = argc > 2 ? atoi(argv[2]) : 4;
    int ngpu = 0;
    CK(cudaGetDeviceCount(&ngpu));
    if (ngpu < 2) { fprintf(stderr, "need >= 2 GPUs, have %d\n", ngpu); return 1; }

    std::vector<int*> buf(ngpu);
    for (int d = 0; d < ngpu; ++d) {
        CK(cudaSetDevice(d));
        CK(cudaMalloc(&buf[d], n * sizeof(int)));
        CK(cudaMemset(buf[d], 0, n * sizeof(int)));
        int peer = (d + 1) % ngpu;
        int can = 0; CK(cudaDeviceCanAccessPeer(&can, d, peer));
        if (can) cudaDeviceEnablePeerAccess(peer, 0);  // idempotent-safe
    }

    dim3 block(256), grid((n + 255) / 256);
    for (int it = 0; it < iters; ++it)
        for (int d = 0; d < ngpu; ++d) {
            CK(cudaSetDevice(d));
            int peer = (d + 1) % ngpu;
            peer_rw<<<grid, block>>>(buf[peer], buf[d], n, it);  // store->peer, load<-local
        }
    for (int d = 0; d < ngpu; ++d) { CK(cudaSetDevice(d)); CK(cudaDeviceSynchronize()); }

    printf("p2p_kernel OK: %d GPUs, %d elems, %d iters (ring peer writes)\n",
           ngpu, n, iters);
    return 0;
}
