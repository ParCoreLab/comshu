#!/bin/bash
#SBATCH --job-name=pcomm-ucxcheck
#SBATCH --account=etur02
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --gres=gpu:1
#SBATCH --time=00:10:00
#SBATCH --qos=acc_debug
#SBATCH --output=ucxcheck-%j.out
#SBATCH --error=ucxcheck-%j.err
#
# PROBE — does anything on this machine actually route through UCX or libfabric?
#
# Not a suite case. It answers one question and then either becomes a provider
# or gets deleted. See plans/PLAN-20260813-2238.
#
# THE QUESTION. A coverage diff against SigmaTrace says it records two whole
# transports we do not: libfabric and UCX. Building either is roughly the size
# of the ibverbs provider, so the decision is worth five minutes of machine time
# rather than a week of guessing.
#
# WHAT A PROVIDER FOR THEM WOULD ACTUALLY BUY, because this is the part that is
# easy to get backwards: **we already capture every verbs operation, whoever
# issued it.** UCX and libfabric both terminate on verbs queue pairs, so their
# traffic is already in the corpus as ordinary ib_post_send / ib_poll_cq rows. A
# provider adds ATTRIBUTION — that a given post_send came from ucp_tag_send
# rather than NCCL's net plugin — and nothing else. If nothing here loads them,
# that attribution has nothing to attribute and both providers are dead weight.
#
# WHY ldd IS NOT THE ANSWER. OpenMPI selects its PML at runtime and dlopens the
# transport, so the launched binary's DT_NEEDED says nothing. The only honest
# test is what the process has MAPPED once MPI is up, which is what this reads
# out of /proc/self/maps from inside an MPI rank.
#
# READING THE RESULT:
#   libucp mapped        -> MPI traffic goes through UCX; a UCX provider would
#                           attribute whatever share of the corpus is MPI
#   libfabric mapped     -> same argument for the libfabric provider
#   neither mapped       -> both providers are dead weight for this science;
#                           the gap is real and irrelevant, and Stage B/C of the
#                           plan should be closed rather than scheduled
#
# It also prints what the NVSHMEM and NCCL workloads map, because those are the
# ones that actually move the bytes — a stack where only the MPI bootstrap
# touches UCX is a very different case from one where the data path does.
set -uo pipefail

echo "=== ucxcheck: which transports are actually mapped ==="
echo "job   : ${SLURM_JOB_ID:-none}"
echo "nodes : ${SLURM_JOB_NODELIST:-?}"
echo "date  : $(date -Is)"

# The suite's own environment, not a hand-rolled one. The first attempt loaded
# modules and assumed mpirun/ompi_info would be on PATH; they were not, and the
# probe reported "mpirun failed" having measured nothing. common.sh is what
# every case uses to find the HPC SDK, so use it.
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
# shellcheck disable=SC1091
. "$PCOMM_ROOT/tests/hpc/suite/common.sh" 2>/dev/null || {
    module load gcc/13.2.0 nvidia-hpc-sdk/25.7 >/dev/null 2>&1 || true
}
MPI_BIN="$(dirname "$(command -v mpirun 2>/dev/null || echo /nonexistent)")"
if [ ! -x "$MPI_BIN/mpirun" ]; then
    for c in "$PCOMM_SDK"/comm_libs/mpi/bin /apps/ACC/NVIDIA-HPC-SDK/*/Linux_x86_64/*/comm_libs/mpi/bin; do
        [ -x "$c/mpirun" ] && MPI_BIN="$c"
    done
fi
export PATH="$MPI_BIN:$PATH"
echo "mpirun : $(command -v mpirun || echo NOT-FOUND)"
echo "mpicc  : $(command -v mpicc  || echo NOT-FOUND)"

echo
echo "--- 1. what OpenMPI was BUILT with -------------------------------------"
# Necessary but not sufficient: built-in support does not mean it is selected.
if command -v ompi_info >/dev/null 2>&1; then
    ompi_info --parsable 2>/dev/null | grep -iE 'mca:(pml|btl|mtl):' | grep -iE 'ucx|ofi|libfabric' \
        || echo "  (no ucx/ofi component reported by ompi_info)"
    echo "  --- pml/mtl components present:"
    ompi_info 2>/dev/null | grep -iE 'MCA (pml|mtl|btl):' | sed 's/^/  /' | head -20
else
    echo "  ompi_info not on PATH"
fi

echo
echo "--- 2. what an MPI RANK actually maps -----------------------------------"
# The decisive measurement. A tiny MPI program is not needed: mpirun launching
# /bin/bash is still a full MPI rank as far as the runtime is concerned only if
# it initialises MPI, which bash does not — so this runs a real MPI_Init via a
# compiled probe.
# SHARED STORAGE, NOT /tmp. /tmp is node-local, so a binary built here and
# launched with mpirun across two nodes fails on the second with "could not
# access an executable" — which is what the first run of this probe did.
PROBE_DIR="${SLURM_SUBMIT_DIR:-$HOME}/.ucxprobe-${SLURM_JOB_ID:-$$}"
mkdir -p "$PROBE_DIR"
cat > "$PROBE_DIR/probe.c" <<'EOF'
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    /* Move a real message first: some transports are only wired up on first
       use, so mapping the library at MPI_Init time is not guaranteed. */
    long buf = rank;
    long sum = 0;
    MPI_Allreduce(&buf, &sum, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0) {
        FILE* m = fopen("/proc/self/maps", "r");
        char line[4096];
        int seen_ucp = 0, seen_ucs = 0, seen_uct = 0, seen_fi = 0, seen_verbs = 0, seen_mlx5 = 0;
        while (m && fgets(line, sizeof line, m)) {
            if (strstr(line, "libucp"))     seen_ucp = 1;
            if (strstr(line, "libucs"))     seen_ucs = 1;
            if (strstr(line, "libuct"))     seen_uct = 1;
            if (strstr(line, "libfabric"))  seen_fi = 1;
            if (strstr(line, "libibverbs")) seen_verbs = 1;
            if (strstr(line, "libmlx5"))    seen_mlx5 = 1;
        }
        if (m) fclose(m);
        printf("  MPI ranks      : %d (allreduce sum=%ld)\n", size, sum);
        printf("  libucp  mapped : %s\n", seen_ucp   ? "YES" : "no");
        printf("  libucs  mapped : %s\n", seen_ucs   ? "YES" : "no");
        printf("  libuct  mapped : %s\n", seen_uct   ? "YES" : "no");
        printf("  libfabric      : %s\n", seen_fi    ? "YES" : "no");
        printf("  libibverbs     : %s\n", seen_verbs ? "YES" : "no");
        printf("  libmlx5        : %s\n", seen_mlx5  ? "YES" : "no");
        printf("\n  VERDICT: ");
        if (seen_ucp)      printf("UCX IS on the MPI path — a UCX provider would attribute real traffic\n");
        else if (seen_fi)  printf("libfabric IS on the MPI path — the libfabric provider is the one to build\n");
        else               printf("NEITHER UCX NOR LIBFABRIC is mapped — both providers are dead weight here\n");
    }
    MPI_Finalize();
    return 0;
}
EOF
# OMPI_CC=gcc because the SDK's mpicc wrapper defaults to nvc, which is not on
# PATH here — the wrapper then refuses to compile anything and the decisive
# measurement silently does not happen. gcc is enough for MPI_Init + Allreduce.
export OMPI_CC="${OMPI_CC:-gcc}"
if mpicc -o "$PROBE_DIR/probe" "$PROBE_DIR/probe.c" 2>"$PROBE_DIR/cc.log"; then
    mpirun -np 2 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH "$PROBE_DIR/probe" \
        || { echo "  !! mpirun failed:"; }
else
    echo "  !! mpicc failed:"; sed 's/^/    /' "$PROBE_DIR/cc.log" | head -20
fi

echo
echo "--- 3. what the DATA-PATH workloads map ---------------------------------"
# MPI may only be the bootstrap. NVSHMEM and NCCL are what move the bytes, so
# ask the same question of them: if they map UCX too, attribution matters much
# more than if only the launcher does.
# Bounded lookups only. The first version ran `find /gpfs/apps ...` per library,
# which on a parallel filesystem this size did not finish inside the job at all —
# the probe hit its time limit having printed nothing for this section. ldconfig
# and the SDK's own lib dirs answer the same question in milliseconds.
for lib in libucp libucs libfabric libibverbs libmlx5 libnccl libnvshmem; do
    hit=$(ldconfig -p 2>/dev/null | grep -m1 "$lib\." | sed 's/^[[:space:]]*//')
    if [ -n "$hit" ]; then
        echo "  $lib: $hit"
        continue
    fi
    hit=$(ls -1 "$PCOMM_SDK"/comm_libs/*/"$lib"*.so* "$PCOMM_SDK"/math_libs/lib64/"$lib"*.so* \
                 /usr/lib64/"$lib"*.so* 2>/dev/null | head -1)
    echo "  $lib: ${hit:-not found (ldconfig, SDK comm_libs, /usr/lib64)}"
done

rm -rf "$PROBE_DIR"
echo
echo "=== ucxcheck done ==="
