#!/bin/bash
# Corpus scale capture — the 64-rank IB star, WITH kernel lifetimes.
#
# `scale_hubspoke_ib.sbatch` at 64 ranks is the widest capture we have and the
# only one whose asymmetry is visible at scale — and it has no compute in it at
# all. Every timeline drawn from it can say rank 17 went quiet and can never say
# whether it was computing or blocked. This job is that job with kernel spans:
#
#   -p ib,cupti   instead of   -p ib
#
#   sbatch tests/hpc/corpus/scale_hubspoke_ib_kt.sbatch
#
# NO NVBIT, DELIBERATELY, AND THE BUILD ENFORCES IT. CUPTI permits one
# subscriber per process and NVBit (which gpumem loads) dlopens libcupti and
# takes it; whoever loses, loses SILENTLY — job 44546506 arm E produced four
# rank directories and zero .pcs files. This runs against `build-cupti`, which
# is configured PCOMM_PROVIDER_GPUMEM=OFF, so there is no NVBit anywhere in the
# process and the conflict cannot arise. `pcomm run` would drop cupti anyway if
# gpumem were selected (run.cpp build_passes), but a build that cannot produce
# the conflicting .so is the stronger guarantee. See
# plans/PLAN-20260813-0600-kernel-lifetimes-belong-to-the-engine.
#
# ITERS 3 -> 20, and that is the one workload change. Kernel launches per rank
# are edges x iters, so at 3 iterations a spoke contributes three spans and the
# lane this job exists to fill is empty. 20 gives ~20 per spoke and ~1260 on the
# hub. Both committed asserts are closed-form in params.ranks and say nothing
# about iteration count, so they carry over verbatim; the byte totals do not
# compare to scale_hubspoke_ib-44542856 and are not meant to.
#
#SBATCH --job-name=pcomm-corpus-scale-hubspoke-ib-kt
#SBATCH --account=etur02
#SBATCH --qos=acc_ehpc
#SBATCH --nodes=16
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:30:00
#SBATCH --output=%x.%j.out

set -x

# The kernel-timing tree, not ~/proj/pcomm: that clone is the committed-code
# transport and this work is not committed yet. build-cupti is the only build
# with libcupti_trace.so in it.
export PCOMM_ROOT="${PCOMM_ROOT:-$HOME/pcomm-impl}"
export PCOMM_BUILD="${PCOMM_BUILD:-$PCOMM_ROOT/build-cupti}"
# Prepended, so common.sh's own list (which it appends) does not hide it.
export PCOMM_PROVIDER_PATH="$PCOMM_BUILD/gpu-backends/cupti"

source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

# libcupti itself. The backend dlopens nothing — it links libcupti — so the
# loader has to find it, and it lives outside the SDK's lib dirs.
_pcomm_module load cuda/12.6
CUPTI_LIB=$(ls -d "$CUDA_HOME"/extras/CUPTI/lib64 2>/dev/null || true)
[ -n "$CUPTI_LIB" ] && export LD_LIBRARY_PATH="$CUPTI_LIB:$LD_LIBRARY_PATH"
echo "=== cupti lib: ${CUPTI_LIB:-NOT FOUND}"
ls -la "$PCOMM_BUILD/gpu-backends/cupti/libcupti_trace.so" || {
  echo "!! no libcupti_trace.so — build-cupti was not built with PCOMM_CUPTI=ON" >&2
  exit 1
}

# Same arch-in-the-name discipline as the parent job: a cached binary from the
# other machine is not a build error, it is a silent wrong answer.
PCOMM_SM=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
PCOMM_SM=${PCOMM_SM:-90}
APP="$HOME/asym_patterns-sm$PCOMM_SM"

if [ ! -e "$NVSHMEM_HOME/lib/libnvshmem_host.so" ]; then
  echo "!! no libnvshmem_host.so under $NVSHMEM_HOME/lib" >&2
  ls "$NVSHMEM_HOME/lib" 2>/dev/null | head -20 >&2
  exit 1
fi

if [ ! -x "$APP" ] || [ "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -nt "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_$PCOMM_SM,code=sm_$PCOMM_SM -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -Xlinker --disable-new-dtags -Xlinker -rpath -Xlinker "$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart \
    || { echo "!! nvcc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi

# Unconditionally, AFTER the build block, and LD_LIBRARY_PATH after that — the
# ordering case 16 documents at length: pcomm_load_build ends by reloading the
# runtime modules, which prepend a different NVSHMEM than the one the binary was
# just linked against.
pcomm_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:${CUPTI_LIB:+$CUPTI_LIB:}$LD_LIBRARY_PATH"
echo "=== mpirun: $(command -v mpirun)"
echo "=== nvshmem this binary links against ==="
ldd "$APP" 2>/dev/null | grep -i nvshmem || echo "  (none reported by ldd)"

OUT=$(pcomm_out scale_hubspoke_ib_kt); rm -rf "$OUT"; mkdir -p "$OUT"

# NVSHMEM_DISABLE_P2P=1 is the line that makes this an IB capture rather than an
# empty one — without it 60 of the 126 edges go over NVLink where an ibverbs
# provider cannot see them. Unchanged from the parent job, and it does NOT
# affect the kernel lane: the put kernels launch either way.
mpirun -np 64 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI -x NVSHMEM_DISABLE_P2P=1 \
  "$PCOMM" run -p ib,cupti -o "$OUT" -- "$APP" hub 4096 20
echo "mpirun exit=$?"

echo "=== captured streams — expect ib_* (100-124) AND gpu_kernel (530) ==="
"$PCOMM" read "$OUT"

# The failure this job is most likely to have is the silent one: everything
# green, no 530 anywhere, because CUPTI never armed. Count it explicitly.
echo "=== rank dirs with a kernel stream ==="
have=0; miss=0
for d in "$OUT"/rank-*; do
  if [ -f "$d/530.pcs" ]; then have=$((have+1)); else miss=$((miss+1)); echo "  no 530: $(basename "$d")"; fi
done
echo "KERNEL_STREAMS have=$have missing=$miss  (missing must be 0)"

"$PCOMM" export "$OUT" --csv "$OUT.csv"

# The parent job's two assertions, verbatim by symlink, at 64 ranks. Both are
# closed-form in params.ranks, so raising iters does not touch them.
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" \
     "$PCOMM_ROOT/tests/hpc/corpus/scale_hubspoke_ib_kt"
