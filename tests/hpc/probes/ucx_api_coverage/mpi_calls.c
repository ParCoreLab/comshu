#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size != 2) {
        if (rank == 0)
            fprintf(stderr, "ucx_api_coverage: expected exactly two ranks, got %d\n", size);
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    enum { ITERS = 100, ELEMS = 1024 };
    unsigned long* buffer = calloc(ELEMS, sizeof(*buffer));
    if (!buffer)
        MPI_Abort(MPI_COMM_WORLD, 3);

    for (int i = 0; i < ITERS; ++i) {
        if (rank == 0) {
            buffer[0] = (unsigned long)i;
            MPI_Send(buffer, ELEMS, MPI_UNSIGNED_LONG, 1, 17, MPI_COMM_WORLD);
            MPI_Recv(buffer, ELEMS, MPI_UNSIGNED_LONG, 1, 18, MPI_COMM_WORLD,
                     MPI_STATUS_IGNORE);
        } else {
            MPI_Recv(buffer, ELEMS, MPI_UNSIGNED_LONG, 0, 17, MPI_COMM_WORLD,
                     MPI_STATUS_IGNORE);
            buffer[0] += 1;
            MPI_Send(buffer, ELEMS, MPI_UNSIGNED_LONG, 0, 18, MPI_COMM_WORLD);
        }
    }

    unsigned long in = (unsigned long)(rank + 1), out = 0;
    MPI_Allreduce(&in, &out, 1, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (out != 3 || buffer[0] != ITERS)
        MPI_Abort(MPI_COMM_WORLD, 4);

    if (rank == 0)
        printf("ucx_api_coverage: PASS ranks=2 iterations=%d bytes_each=%zu\n", ITERS,
               (size_t)ELEMS * sizeof(*buffer));
    free(buffer);
    MPI_Finalize();
    return 0;
}
