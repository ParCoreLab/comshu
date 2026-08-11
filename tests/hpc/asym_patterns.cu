// Asymmetric NVSHMEM communication patterns — hub, pipe, skew.
//
// WHY THIS EXISTS. Every capture in the corpus was a ring or a pair, and that
// made two things invisible at once.
//
//   1. Every comm matrix was symmetric. A ring sends A->B and B->A in equal
//      volume, so nothing in the visualiser could distinguish "we record
//      direction" from "we symmetrise and nobody noticed". A directional
//      pattern with zero back-flow (pipe) is the test for that.
//   2. Every object heatmap was FLAT. Measured on the 4-GPU ring: 512 accesses
//      in every one of 32 bins, on every object, in every capture that has
//      gpu_access at all. Every workload wrote its whole buffer uniformly, so
//      the object view's core figure had never once shown structure — it was
//      rendering a solid bar and we had no way to tell a correct heatmap from a
//      broken one.
//
// The hub pattern fixes (2) as a side effect of fixing (1): each spoke writes
// its OWN CONTIGUOUS SLICE of the hub's receive buffer, so the hub's object is
// one buffer written by several different GPUs in disjoint regions. That is a
// banded heatmap, a `touched by` listing several GPUs, and a partially covered
// object — three things no existing capture produces.
//
// All three patterns are one kernel (`put_slice`) driven by a per-PE send list,
// because the pattern is data and only the data differs. PCOMM_KERNEL_NAME can
// therefore filter on one name for all three.
//
// NB it is NOT inlined here, contrary to what case 04's job.sbatch says about
// its own binary. PCOMM_INSTR_DUMP on this workload shows two instrumented
// functions: `put_slice` with 3 instructions and
// `nvshmemi_transfer_rma_p<int>` with 1485. So `nvshmem_int_p` compiles to a
// real call in this build. Whether it inlines is a property of the build, not
// of the source, and the filter's reach differs accordingly — worth knowing
// before reasoning about what a kernel-name filter does or does not capture.
//
//   asym_patterns <hub|pipe|skew> [slot_elems] [iters]
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <cuda_runtime.h>
#include <mpi.h>
#include <nvshmem.h>
#include <nvshmemx.h>

// EVERY INDEX HERE IS 32-BIT, DELIBERATELY, AND IT IS NOT A STYLE CHOICE.
//
// Instrumenting this kernel with a 64-bit index computation raises an illegal
// memory access that poisons the CUDA context — the workload runs perfectly with
// no profiler. Bisected to the instruction with PCOMM_INSTR_LO/HI (MN5 jobs
// 44464306, 44464689) and then to the cause with four kernel variants (44464836,
// 44465353):
//
//   long i, long dst_off                       -> FAULT
//   int  i, long dst_off  (dst_off + i is 64b) -> FAULT
//   long i, no offset                          -> FAULT
//   int  i, no offset                          -> clean
//   int  i, int  dst_off                       -> clean   <- this kernel
//
// Every variant that computes a 64-bit index into the symmetric-heap pointer
// faults; every 32-bit one is clean. It is not the offset — variant 3 has none
// and still faults on a `long` counter — and it is not NVSHMEM's code, whose
// 1485 instrumented instructions in nvshmemi_transfer_rma_p are all clean while
// each of this kernel's three faults on its own.
//
// This is a WORKAROUND in the fixture, not a fix. The defect is in the
// instrumentation of 64-bit-indexed accesses and it still bites any real
// application indexing past 2 GB. See PLAN-20260810-0854 and
// things-to-circle-back-to.md.
//
// Case 04 passes for exactly this reason and no other: `put_to_next` happens to
// use `int i` and `&dst[i]`.
__global__ void put_slice(int* dst, const int* src, int n, int peer, int dst_off) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        nvshmem_int_p(&dst[dst_off + i], src[i], peer);
}

// One outgoing transfer. `dst` is a symmetric address, so it names the same
// offset in every PE's heap and `peer` picks whose.
struct Send {
    int  peer;
    int* dst;
    int  dst_off;  // 32-bit on purpose — see put_slice above
    int  count;
};

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

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

    const char* pattern = argc > 1 ? argv[1] : "hub";
    const long  slot = argc > 2 ? atol(argv[2]) : 4096;  // ints per sender slot
    const int   iters = argc > 3 ? atoi(argv[3]) : 3;

    // Three DIFFERENT sizes, on purpose: an object table where every row is the
    // same size cannot show that size is a column worth having. Every PE
    // allocates all three identically — the symmetric heap requires it — and
    // the sizes come only from argv, so they are identical by construction.
    const long recv_elems = (long)npes * slot;  // one slot per possible sender
    const long src_elems = slot;
    const long resp_elems = slot / 8;

    // The kernel indexes with int (see put_slice). Every offset it can be given
    // is < recv_elems, so that is the bound to check — and checking beats a
    // comment, because the failure it prevents is a silent wrong answer rather
    // than a crash.
    if (recv_elems > 2147483647L) {
        if (mype == 0)
            fprintf(stderr, "asym_patterns: slot too large — npes*slot must fit in int\n");
        MPI_Abort(MPI_COMM_WORLD, 3);
    }

    int* recv = static_cast<int*>(nvshmem_malloc(recv_elems * sizeof(int)));
    int* src = static_cast<int*>(nvshmem_malloc(src_elems * sizeof(int)));
    int* resp = static_cast<int*>(nvshmem_malloc(resp_elems * sizeof(int)));
    if (!recv || !src || !resp) {
        if (mype == 0)
            fprintf(stderr, "asym_patterns: nvshmem_malloc failed\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    cudaMemset(src, 1, src_elems * sizeof(int));

    std::vector<Send> sends;

    if (strcmp(pattern, "hub") == 0) {
        // Star. Spokes push a full slot to PE 0; PE 0 answers each spoke with
        // an eighth of one. The volume asymmetry is the point: an aggregate
        // that symmetrises an edge reports the answer flow as a full slot.
        //
        // dst_off = mype * slot gives every spoke its own disjoint region of
        // the hub's buffer, which is what makes the hub's object heatmap
        // banded rather than solid.
        if (mype != 0)
            sends.push_back({0, recv, (int)(mype * slot), (int)slot});
        else
            for (int s = 1; s < npes; ++s)
                sends.push_back({s, resp, 0, (int)resp_elems});
    } else if (strcmp(pattern, "pipe") == 0) {
        // Chain. PE i sends only to i+1, and the last PE sends nothing at all,
        // so the matrix is a single superdiagonal with a structurally empty
        // last row. Volume grows along the chain, so the diagonal is also a
        // gradient rather than a constant — a stage that dropped its send is
        // visible as a gap, not just as a smaller number.
        if (mype + 1 < npes) {
            const long n = slot * (mype + 1) / npes;
            sends.push_back({mype + 1, recv, (int)(mype * slot), (int)n});
        }
    } else if (strcmp(pattern, "skew") == 0) {
        // Sparse and heavy-tailed, from a closed form rather than an RNG so the
        // edge set is identical on every run and on every PE count — a capture
        // you cannot reproduce is not a fixture. Roughly half the pairs carry
        // no edge, and the ones that do span a 32x range, which is what puts
        // the log colour scale under load.
        for (int peer = 0; peer < npes; ++peer) {
            if (peer == mype)
                continue;
            const int h = (mype * 31 + peer * 17) % 11;
            if (h < 5)
                continue;
            const long n = slot >> (h - 5);  // slot/1 .. slot/32
            if (n > 0)
                sends.push_back({peer, recv, (int)(mype * slot), (int)n});
        }
    } else {
        if (mype == 0)
            fprintf(stderr, "asym_patterns: unknown pattern '%s'\n", pattern);
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    for (int it = 0; it < iters; ++it) {
        for (const Send& s : sends) {
            dim3 block(256), grid((unsigned)((s.count + 255) / 256));
            put_slice<<<grid, block>>>(s.dst, src, s.count, s.peer, s.dst_off);
        }
        nvshmemx_barrier_all_on_stream(0);
    }
    cudaDeviceSynchronize();
    nvshmem_barrier_all();

    // Per-PE send totals, printed by every PE so the expected matrix is in the
    // job log next to the captured one. Cheap, and it has already been the
    // fastest way to tell "the workload did not send" from "we did not record".
    long total = 0;
    for (const Send& s : sends)
        total += s.count;
    printf("asym_patterns[%s] PE %d/%d: %zu edges, %ld elems/iter, %d iters\n", pattern, mype, npes,
           sends.size(), total, iters);
    fflush(stdout);

    nvshmem_free(resp);
    nvshmem_free(src);
    nvshmem_free(recv);
    nvshmem_finalize();
    MPI_Finalize();
    return 0;
}
