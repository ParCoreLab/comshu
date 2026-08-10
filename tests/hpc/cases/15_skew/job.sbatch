#!/bin/bash
# Case 15 — skew: NVSHMEM sparse heavy-tailed pairs, edge weights spanning 32x
#
# Shares tests/hpc/asym_patterns.cu with the other two asymmetric-pattern cases;
# only the pattern argument differs. See that file for why these exist — briefly,
# every capture in the corpus was a ring or a pair, so every comm matrix was
# symmetric and every object heatmap was flat, and neither of those could be
# told apart from a bug.
#
#   sbatch tests/hpc/cases/15_skew/job.sbatch
#
#SBATCH --job-name=pcomm-15-skew
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

# Compute capability from the device rather than hardcoded, because these three
# cases are meant to run on MN5's H100s (sm_90) AND on the A100 box (sm_80),
# where run_on_a100.sh executes this file directly. A fixed -gencode is why the
# older cases are MN5-only.
# PCOMM_SM, not CC: `CC` is the C compiler variable, and pcomm_load_build below
# loads modules that export it. The first version used CC and was clobbered
# BETWEEN its two uses — the binary was named asym_patterns-sm90 (correct, read
# before the modules loaded) while -gencode became
# arch=compute_/apps/.../bin/nvc (read after). nvcc rejected it and the case
# then ran a binary that did not exist, exit 127.
PCOMM_SM=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
PCOMM_SM=${PCOMM_SM:-90}
APP="$HOME/asym_patterns-sm$PCOMM_SM"

# NVSHMEM_HOME is resolved by globbing the SDKs and taking the newest, so it can
# name a directory whose layout differs from the one the modules provide. If the
# host library is not where we are about to point -L, the link silently falls
# back to whatever the module put on the default search path and the mismatch
# only surfaces as an undefined symbol at startup — which is how job 44457910
# was spent. Check it here, where the message can say which directory.
if [ ! -e "$NVSHMEM_HOME/lib/libnvshmem_host.so" ]; then
  echo "!! no libnvshmem_host.so under $NVSHMEM_HOME/lib" >&2
  ls "$NVSHMEM_HOME/lib" 2>/dev/null | head -20 >&2
  exit 1
fi

# One binary per arch for all three cases, so the three captures differ only in
# the pattern and never in the build. The arch is in the name because a cached
# binary from the other machine is not a build error, it is a silent wrong
# answer.
if [ ! -x "$APP" ] || [ "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -nt "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_$PCOMM_SM,code=sm_$PCOMM_SM -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -Wl,--disable-new-dtags,-rpath,"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart \
    || { echo "!! nvcc failed — refusing to run a stale or missing binary" >&2; exit 1; }
  pcomm_load_runtime
fi

# WHICH NVSHMEM this binary actually resolves, printed every run.
#
# Job 44457910 died at startup with "undefined symbol:
# nvshmem_selected_device_transport, version NVSHMEM". NVSHMEM_HOME is resolved
# by globbing the SDKs and taking the newest (26.5 on MN5), while
# pcomm_load_runtime does `module load nvidia-hpc-sdk/25.7` — so the headers and
# -L can come from one NVSHMEM and the loader can find another. Case 04 does not
# hit this only because its binary is cached from before the two diverged.
#
# -rpath above pins the binary to the NVSHMEM it was linked against, which is
# the fix. This line is the evidence: a version mismatch should be one grep in
# the log, not another job.
echo "=== nvshmem this binary links against ==="
ldd "$APP" 2>/dev/null | grep -i nvshmem || echo "  (none reported by ldd)"

OUT=$(pcomm_out skew); rm -rf "$OUT"; mkdir -p "$OUT"

# PCOMM_KERNEL_NAME=put_slice for the same reason as case 04: nvshmem_int_p is
# inlined into put_slice and captured as part of its SASS, and the filter's job
# is to exclude NVSHMEM's own separately-launched barrier kernels, which are
# enormous and not byte-repeatable.
mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
  -x PCOMM_KERNEL_NAME=put_slice \
  "$PCOMM" -p p2p -o "$OUT" -- "$APP" skew 4096 3
echo "mpirun exit=$?"

echo "=== captured streams ==="; "$PCOMM_READ" "$OUT"
"$PCOMM_READ" "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/cases/15_skew"
