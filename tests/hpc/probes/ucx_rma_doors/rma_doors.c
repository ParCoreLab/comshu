// The MPI calls that reach UCX's one-sided and legacy doors, and nothing else.
//
// WHY A SECOND RMA WORKLOAD, when 21_ucx_deep already does Put and Get. That
// case exists to make UCX choose different TRANSPORT protocols by size, and its
// assertions are pinned to the sizes and thresholds it sets. This one exists to
// make MPI reach different UCP DOORS, which is a different axis: the doors below
// were hooked on a static ELF survey of MN5's HPC-X and, apart from Put and Get,
// have never been observed firing on hardware at all.
//
// The four groups, and the door each is here to reach:
//
//   MPI_Put / MPI_Get      -> ucp_put_nbi / ucp_get_nbi        (UcxDoor::Nbi)
//   MPI_Accumulate         -> ucp_atomic_post / _fetch_nb      (UcxAtomic)
//   MPI_Fetch_and_op       -> the fetching atomic door
//   MPI_Ssend              -> ucp_tag_send_sync_nb             (LegacySync)
//
// and all four of them name a remote key, which is the point: every ucx_rma and
// ucx_atomic row interns a ucp_rkey_h, and until UcxRkey existed that handle
// joined to nothing at all.
//
// MPI_UNSIGNED_CHAR FOR THE TRANSFERS, for size_sweep.c's reason: UcxTagSend
// .count is a UCP datatype count and equals bytes only for a contiguous 8-bit
// datatype. Choosing the datatype that makes the two coincide is what lets an
// assertion compare a recorded count against a size at all.
//
// MPI_UINT64_T FOR THE ATOMICS, which is a different constraint pulling the
// other way and not an inconsistency. UcxAtomic.op_size says the operand is
// "4 or 8" bytes; a one-byte atomic is not a narrow atomic, it is an invalid
// one, and job 45177421 proved it by failing all twenty with UCS status -5.
// See the atomic loop below for what that run cost and what it revealed.
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CHECK(call)                                                                 \
    do {                                                                            \
        int _e = (call);                                                            \
        if (_e != MPI_SUCCESS) {                                                    \
            fprintf(stderr, "rma_doors: %s failed at line %d\n", #call, __LINE__);   \
            MPI_Abort(MPI_COMM_WORLD, 1);                                           \
        }                                                                           \
    } while (0)

static long env_long(const char* name, long dflt) {
    const char* v = getenv(name);
    return (v && *v) ? strtol(v, NULL, 0) : dflt;
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size != 2) {
        if (rank == 0)
            fprintf(stderr, "rma_doors: needs exactly 2 ranks, got %d\n", size);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    const long iters     = env_long("RMA_ITERS", 10);
    const long rma_bytes = env_long("RMA_BYTES", 4096);
    const long win_bytes = env_long("WIN_BYTES", 65536);
    const long ssends    = env_long("SSEND_ITERS", 10);
    const long ssend_len = env_long("SSEND_BYTES", 64);

    if (rma_bytes > win_bytes || ssend_len < 1 || (win_bytes % 8) != 0) {
        if (rank == 0)
            fprintf(stderr, "rma_doors: bad sizes\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    // The window is the memory the peer will reach into. MPI_Win_allocate rather
    // than Win_create so the MPI layer registers memory it owns, which is the
    // path mca_osc_ucx takes to ucp_mem_map and then to ucp_rkey_pack.
    unsigned char* win_base = NULL;
    MPI_Win        win;
    CHECK(MPI_Win_allocate((MPI_Aint)win_bytes, 1, MPI_INFO_NULL, MPI_COMM_WORLD,
                           &win_base, &win));
    memset(win_base, 0, (size_t)win_bytes);

    unsigned char* local = malloc((size_t)rma_bytes);
    if (!local) MPI_Abort(MPI_COMM_WORLD, 1);
    memset(local, (int)(rank + 1), (size_t)rma_bytes);

    const int peer = 1 - rank;

    // --- Put and Get, the _nbi doors ----------------------------------------
    // A DIFFERENT OFFSET each round, so a capture can tell the rounds apart by
    // remote_addr alone without joining on size -- 21_ucx_deep's convention,
    // kept deliberately so the two cases read the same way.
    CHECK(MPI_Win_fence(0, win));
    for (long i = 0; i < iters; ++i) {
        MPI_Aint off = (MPI_Aint)((i * rma_bytes) % (win_bytes - rma_bytes + 1));
        CHECK(MPI_Put(local, (int)rma_bytes, MPI_UNSIGNED_CHAR, peer, off,
                      (int)rma_bytes, MPI_UNSIGNED_CHAR, win));
    }
    CHECK(MPI_Win_fence(0, win));
    for (long i = 0; i < iters; ++i) {
        MPI_Aint off = (MPI_Aint)((i * rma_bytes) % (win_bytes - rma_bytes + 1));
        CHECK(MPI_Get(local, (int)rma_bytes, MPI_UNSIGNED_CHAR, peer, off,
                      (int)rma_bytes, MPI_UNSIGNED_CHAR, win));
    }
    CHECK(MPI_Win_fence(0, win));

    // --- Accumulate and Fetch_and_op, the atomic doors ----------------------
    // Single element, contiguous, so this is the shape UCX can turn into a
    // remote atomic.
    //
    // EIGHT BYTES, AND THE WIDTH IS NOT A DETAIL. UcxAtomic.op_size says the
    // operand is "4 or 8", and UCX means it: job 45177421 ran this loop on
    // MPI_UNSIGNED_CHAR and every one of the twenty operations came back
    // `op_size=1 status=-5 status_class=error`, with UCX printing "invalid
    // atomic operation datatype: 0x8" once per call. The capture was correct —
    // the provider recorded the failures faithfully — but the WORKLOAD was
    // asking for something no transport implements, so the doors were reached
    // and never actually taken.
    //
    // It also nearly passed for the wrong reason, which is the part worth
    // remembering: `ucx_atomic` was not empty in that run. It held 80 successful
    // 8-byte cswap and swap rows issued by mca_osc_ucx's OWN window
    // synchronisation, so an assertion that merely required the table to be
    // non-empty would have gone green over a workload whose every atomic failed.
    // That is why the assertion beside this file requires a successful `add`.
    //
    // The target is the LAST ALIGNED SLOT of the window, chosen so it cannot
    // overlap any Put/Get offset above — those run to 40960 — and so an
    // assertion can separate the two families by remote_addr alone.
    uint64_t       one = 1, fetched = 0;
    const MPI_Aint atomic_off = (MPI_Aint)(win_bytes - 8);
    for (long i = 0; i < iters; ++i) {
        CHECK(MPI_Accumulate(&one, 1, MPI_UINT64_T, peer, atomic_off, 1,
                             MPI_UINT64_T, MPI_SUM, win));
    }
    CHECK(MPI_Win_fence(0, win));
    for (long i = 0; i < iters; ++i) {
        CHECK(MPI_Fetch_and_op(&one, &fetched, MPI_UINT64_T, peer, atomic_off,
                               MPI_SUM, win));
    }
    CHECK(MPI_Win_fence(0, win));

    // --- Ssend, the legacy synchronous tagged door --------------------------
    // NOT a duplicate of an ordinary send: ucp_tag_send_sync_nb completes only
    // once the receiver has matched, so folding the two together would erase the
    // difference between a send that buffered and one that waited for its peer.
    // UcxDoor keeps them apart and nothing had ever driven this door on
    // hardware.
    unsigned char* sbuf = malloc((size_t)ssend_len);
    unsigned char* rbuf = malloc((size_t)ssend_len);
    if (!sbuf || !rbuf) MPI_Abort(MPI_COMM_WORLD, 1);
    memset(sbuf, (int)(rank + 1), (size_t)ssend_len);
    for (long i = 0; i < ssends; ++i) {
        if (rank == 0) {
            CHECK(MPI_Ssend(sbuf, (int)ssend_len, MPI_UNSIGNED_CHAR, peer, 909,
                            MPI_COMM_WORLD));
            CHECK(MPI_Recv(rbuf, (int)ssend_len, MPI_UNSIGNED_CHAR, peer, 910,
                           MPI_COMM_WORLD, MPI_STATUS_IGNORE));
        } else {
            CHECK(MPI_Recv(rbuf, (int)ssend_len, MPI_UNSIGNED_CHAR, peer, 909,
                           MPI_COMM_WORLD, MPI_STATUS_IGNORE));
            CHECK(MPI_Ssend(sbuf, (int)ssend_len, MPI_UNSIGNED_CHAR, peer, 910,
                            MPI_COMM_WORLD));
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0)
        printf("rma_doors: OK iters=%ld rma_bytes=%ld win_bytes=%ld ssends=%ld\n",
               iters, rma_bytes, win_bytes, ssends);

    free(sbuf);
    free(rbuf);
    free(local);
    MPI_Win_free(&win);
    MPI_Finalize();
    return 0;
}
