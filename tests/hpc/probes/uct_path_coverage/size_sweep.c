// A workload chosen to reach every UCT protocol, not to be a benchmark.
//
// MN5 job 45113099 proved the ucx_deep provider sees the UCT layer at all: 295
// operations, every one an active message. That left three of the four families
// in ucx_deep_records.hpp with no hardware evidence whatsoever — zero-copy was
// four rows, rendezvous was zero, hardware tag matching was zero — and a record
// field nothing has ever populated is a field nobody has checked the meaning of.
//
// UCX picks its protocol BY MESSAGE SIZE against three thresholds, so the
// workload is a size sweep and the thresholds are pinned by the job script
// rather than left to whatever the stack defaults to:
//
//   size <  UCX_ZCOPY_THRESH   inline or bcopy — copy into a bounce buffer
//   size >= UCX_ZCOPY_THRESH   zcopy — register the user buffer and DMA from it
//   size >= UCX_RNDV_THRESH    rendezvous — advertise, let the peer pull
//   size >= UCX_TM_THRESH      offloaded to the NIC's tag matcher, if enabled
//
// One rank per node, for case 19's reason: UCX picks shared memory for a
// same-node pair, and a shm-only run exercises the doors while saying nothing
// about the transport this case exists to observe.
//
// MPI_UNSIGNED_CHAR THROUGHOUT, deliberately. `UcxTagSend.count` is a UCP
// datatype count and equals bytes only for a contiguous 8-bit datatype — that
// caveat is why comm_edges reports the UCX lane in 'datatype-units' rather than
// bytes. Choosing the datatype that makes the two coincide is what lets this
// case's assertions compare a recorded count against a size in meta.json at all.
//
// The one-sided half is not decoration: MPI_Put/MPI_Get over osc/ucx are the
// only way an MPI program reaches UCT's Put*/Get* doors, and `remote_addr` —
// the object-heatmap key the whole HPC-X class has never had — is written by
// those doors and by no other.

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Kept in step with meta.json's "workload" block BY HAND, and asserted against
// it: sql_check.sh materialises those values as `params`, so an assertion that
// reads params.msg_max_bytes and finds no operation that large is reporting a
// disagreement between this file and that one.
enum { ITERS = 20, RMA_ITERS = 10, RMA_BYTES = 1u << 20, WIN_BYTES = 4u << 20 };
static const size_t SIZES[] = {8, 4096, 65536, 4194304};
enum { NSIZES = (int)(sizeof SIZES / sizeof *SIZES) };

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size != 2) {
        if (rank == 0)
            fprintf(stderr, "uct_path_coverage: expected exactly two ranks, got %d\n", size);
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    unsigned char* buf = malloc(SIZES[NSIZES - 1]);
    if (!buf)
        MPI_Abort(MPI_COMM_WORLD, 3);
    memset(buf, 0xa5, SIZES[NSIZES - 1]);

    // --- two-sided: the size sweep ------------------------------------------
    //
    // A DIFFERENT TAG PER SIZE, so a capture can attribute an operation to the
    // size that produced it without joining on a byte count that several
    // protocols would report differently. `uct_send.tag` is populated only by
    // the hardware-tag-matching doors, which is exactly what makes it the
    // evidence that those doors were reached.
    for (int s = 0; s < NSIZES; ++s) {
        const size_t n = SIZES[s];
        const int    tag = 100 + s;
        for (int i = 0; i < ITERS; ++i) {
            if (rank == 0) {
                MPI_Send(buf, (int)n, MPI_UNSIGNED_CHAR, 1, tag, MPI_COMM_WORLD);
                MPI_Recv(buf, (int)n, MPI_UNSIGNED_CHAR, 1, tag, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
            } else {
                MPI_Recv(buf, (int)n, MPI_UNSIGNED_CHAR, 0, tag, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
                MPI_Send(buf, (int)n, MPI_UNSIGNED_CHAR, 0, tag, MPI_COMM_WORLD);
            }
        }
    }

    // --- one-sided: the Put*/Get* doors -------------------------------------
    //
    // Rank 0 drives; rank 1 exposes the window. Fences rather than locks: the
    // question is which UCT door the transfer took, and passive-target
    // synchronisation would add a second protocol (and its own AM traffic) to a
    // capture that is about the first.
    unsigned char* win_mem = NULL;
    MPI_Win        win = MPI_WIN_NULL;
    MPI_Alloc_mem(WIN_BYTES, MPI_INFO_NULL, &win_mem);
    if (!win_mem)
        MPI_Abort(MPI_COMM_WORLD, 4);
    memset(win_mem, 0x5a, WIN_BYTES);
    MPI_Win_create(win_mem, WIN_BYTES, 1, MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    for (int i = 0; i < RMA_ITERS; ++i) {
        MPI_Win_fence(0, win);
        if (rank == 0) {
            // A DIFFERENT OFFSET EACH ITERATION, so `remote_addr` varies. A
            // constant target address would make an object heatmap built from
            // this capture look correct while proving nothing about whether the
            // address was recorded or merely defaulted.
            const MPI_Aint off = (MPI_Aint)((size_t)i * RMA_BYTES) % (WIN_BYTES - RMA_BYTES + 1);
            MPI_Put(buf, RMA_BYTES, MPI_UNSIGNED_CHAR, 1, off, RMA_BYTES, MPI_UNSIGNED_CHAR, win);
            MPI_Win_fence(0, win);
            MPI_Get(buf, RMA_BYTES, MPI_UNSIGNED_CHAR, 1, off, RMA_BYTES, MPI_UNSIGNED_CHAR, win);
        } else {
            MPI_Win_fence(0, win);
        }
    }
    MPI_Win_fence(0, win);
    MPI_Win_free(&win);
    MPI_Free_mem(win_mem);

    unsigned long in = (unsigned long)(rank + 1), out = 0;
    MPI_Allreduce(&in, &out, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (out != 3)
        MPI_Abort(MPI_COMM_WORLD, 5);

    if (rank == 0) {
        size_t total = 0;
        for (int s = 0; s < NSIZES; ++s)
            total += SIZES[s] * ITERS;
        printf("uct_path_coverage: PASS ranks=2 sizes=%d iters=%d two_sided_bytes=%zu "
               "rma_iters=%d rma_bytes=%u\n",
               NSIZES, ITERS, total, RMA_ITERS, RMA_BYTES);
    }
    free(buf);
    MPI_Finalize();
    return 0;
}
