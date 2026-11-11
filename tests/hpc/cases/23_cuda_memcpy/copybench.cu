// copybench — a workload whose entire communication is cudaMemcpy.
//
// It exists to be an ORACLE, not a benchmark. Every transfer it performs is
// counted in the source, so meta.json's `workload` block can state the exact
// number of copies and the exact number of bytes in each direction, and the
// case's assertions derive from those rather than from anything the tool
// emitted.
//
// It is also the shape of workload comshu was structurally blind to until
// cuda_memcpy (534) existed: no kernels that touch peer memory (so gpumem/NVBit
// sees nothing), no verbs, no UCX, no NCCL. Before this record a capture of this
// program was an empty communication matrix.
//
//   copybench <iters> <msg_bytes> <ndev>
//
// Phases, per iteration, in this order:
//   1. H2D   pinned host -> each device            ndev copies of msg_bytes
//   2. D2H   each device -> pinned host            ndev copies of msg_bytes
//   3. D2D   device 0 buffer A -> device 0 buffer B      1 copy of msg_bytes
//   4. peer  cudaMemcpyPeer for every ORDERED pair  ndev*(ndev-1) of msg_bytes
//   5. uva   cudaMemcpy(dst on dev 1, src on dev 0, cudaMemcpyDeviceToDevice)
//                                                        1 copy of uva_bytes
//
// PHASE 5 IS A MEASUREMENT, NOT A WORKLOAD. It is the same bytes-across-a-device-
// boundary transfer as phase 4, spelled as a LOCAL copy: under unified
// addressing cudaMemcpy will do it, and CUPTI may report it either as
// CUPTI_ACTIVITY_KIND_MEMCPY2 (two device ordinals, correctly an edge) or as
// CUPTI_ACTIVITY_KIND_MEMCPY with copyKind DTOD (one ordinal, correctly not an
// edge, because one ordinal is all the instrument saw). Which one it is decides
// nothing about the derived layer — comm_edges keys on `src_device <>
// dst_device` and is right under either answer — but it is worth knowing, and
// nothing in the corpus contained the case. It uses its OWN size (uva_bytes,
// half of msg_bytes) so that every assertion about phase 4 can exclude it by
// byte count without depending on how CUPTI classified it.
//
// Everything is SYNCHRONOUS. An async sweep would be a better bandwidth test
// and a worse oracle: the run would have to end with a device sync whose
// placement decides how many copies CUPTI has flushed, and "how many copies did
// this program perform" would stop being a number the source can state.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cuda_runtime.h>

#define CK(expr)                                                                                   \
    do {                                                                                           \
        cudaError_t _e = (expr);                                                                   \
        if (_e != cudaSuccess) {                                                                   \
            std::fprintf(stderr, "copybench: %s:%d: %s: %s\n", __FILE__, __LINE__, #expr,          \
                         cudaGetErrorString(_e));                                                  \
            return 2;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char** argv) {
    const int    iters = argc > 1 ? std::atoi(argv[1]) : 4;
    const size_t msg_bytes = argc > 2 ? (size_t)std::atoll(argv[2]) : (4u << 20);
    int          want_dev = argc > 3 ? std::atoi(argv[3]) : 4;
    const size_t uva_bytes = msg_bytes / 2;

    int have = 0;
    CK(cudaGetDeviceCount(&have));
    if (have < want_dev) {
        // Reported, never silently adjusted: the assertions are derived from
        // the `gpus` parameter in meta.json, so a run on fewer GPUs than the
        // case declares must be visible as a failure of the case's premise
        // rather than as a quietly smaller capture that still passes.
        std::fprintf(stderr, "copybench: asked for %d GPUs, only %d present\n", want_dev, have);
        return 3;
    }
    const int ndev = want_dev;

    // Peer access has to be ENABLED, and a failure here is fatal rather than a
    // fallback. Without it cudaMemcpyPeer still works — it stages through host
    // memory — and CUPTI then reports two ordinary copies instead of one peer
    // copy, so the case would be measuring a different mechanism while looking
    // identical from the outside.
    for (int i = 0; i < ndev; ++i) {
        CK(cudaSetDevice(i));
        for (int j = 0; j < ndev; ++j) {
            if (i == j)
                continue;
            int can = 0;
            CK(cudaDeviceCanAccessPeer(&can, i, j));
            if (!can) {
                std::fprintf(stderr, "copybench: device %d cannot peer-access %d\n", i, j);
                return 4;
            }
            cudaError_t e = cudaDeviceEnablePeerAccess(j, 0);
            if (e != cudaSuccess && e != cudaErrorPeerAccessAlreadyEnabled) {
                std::fprintf(stderr, "copybench: enable peer %d->%d: %s\n", i, j,
                             cudaGetErrorString(e));
                return 4;
            }
        }
    }

    void* host = nullptr;
    CK(cudaHostAlloc(&host, msg_bytes, cudaHostAllocDefault));
    std::memset(host, 0xA5, msg_bytes);

    void* dev[16] = {nullptr};
    for (int i = 0; i < ndev; ++i) {
        CK(cudaSetDevice(i));
        CK(cudaMalloc(&dev[i], msg_bytes));
    }
    // The second buffer on device 0, for the same-device phase. A same-device
    // copy is deliberately in the workload: it is the row that must NOT become
    // an edge, and a case that only performed edge-producing copies could not
    // tell a correct filter from a missing one.
    void* dev0b = nullptr;
    CK(cudaSetDevice(0));
    CK(cudaMalloc(&dev0b, msg_bytes));

    for (int it = 0; it < iters; ++it) {
        for (int i = 0; i < ndev; ++i) {
            CK(cudaSetDevice(i));
            CK(cudaMemcpy(dev[i], host, msg_bytes, cudaMemcpyHostToDevice));
        }
        for (int i = 0; i < ndev; ++i) {
            CK(cudaSetDevice(i));
            CK(cudaMemcpy(host, dev[i], msg_bytes, cudaMemcpyDeviceToHost));
        }
        CK(cudaSetDevice(0));
        CK(cudaMemcpy(dev0b, dev[0], msg_bytes, cudaMemcpyDeviceToDevice));

        for (int i = 0; i < ndev; ++i)
            for (int j = 0; j < ndev; ++j) {
                if (i == j)
                    continue;
                CK(cudaMemcpyPeer(dev[j], j, dev[i], i, msg_bytes));
            }

        if (ndev >= 2) {
            CK(cudaSetDevice(0));
            CK(cudaMemcpy(dev[1], dev[0], uva_bytes, cudaMemcpyDeviceToDevice));
        }
    }

    for (int i = 0; i < ndev; ++i) {
        CK(cudaSetDevice(i));
        CK(cudaDeviceSynchronize());
    }

    const long long peer = (long long)iters * ndev * (ndev - 1);
    std::printf("copybench: iters=%d msg_bytes=%zu uva_bytes=%zu gpus=%d\n", iters, msg_bytes,
                uva_bytes, ndev);
    std::printf("copybench: h2d=%d d2h=%d d2d=%d peer=%lld uva=%d total=%lld\n", iters * ndev,
                iters * ndev, iters, peer, ndev >= 2 ? iters : 0,
                (long long)iters * ndev * 2 + iters + peer + (ndev >= 2 ? iters : 0));
    return 0;
}
