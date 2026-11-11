#!/bin/bash
# PROBE — does instrumenting ONE local load wedge NVSHMEM's IBRC proxy without
# comshu? (things-to-circle-back-to.md: "Instrumenting one local-memory load
# wedges NVSHMEM's IBRC proxy".)
#
# Builds tool.cu + inject.cu as a stand-alone NVBit tool with NVBit's own example
# flags — no libcomshu, no engine, no provider — and runs case 04's putget under
# it on ONE node with NVSHMEM_DISABLE_P2P=1 (IBRC carries every put), which is
# where the hang reproduced. Each arm gets a timeout; 124 = hung.
#
#   bare      no NVBit at all                      the workload's own baseline
#   none      NVBit loaded, target JIT'd, no call  does NVBit's patching alone hang?
#   empty     a call that does nothing             does splicing a call in hang?
#   atomic    a global atomicAdd per lane          a side effect, no channel
#   channel   NVBit's stock channel push           what comshu's push() does
#
# comshu's bisect said: hangs with push, completes with -DCOMSHU_INJECT_NO_PUSH.
# If `channel` hangs here and `empty` does not, the reproducer belongs to
# NVBit/NVSHMEM, with this directory as the report.
#SBATCH --job-name=probe-nvbit-ldl-wedge
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:30:00
#SBATCH --output=%x.%j.out

COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
PROBE="$COMSHU_ROOT/tests/hpc/probes/nvbit_ldl_wedge"
NVBIT="$COMSHU_ROOT/gpu-backends/nvbit/nvbit-1.7.7.1"
WORK="${WEDGE_WORK:-$HOME/wedge-$SLURM_JOB_ID}"; mkdir -p "$WORK"

# ---- build: the tool exactly as NVBit's examples build theirs ---------------
for g in gcc/11.4.0 gcc/12.3.0; do module load $g >/dev/null 2>&1 && break; done
module load nvidia-hpc-sdk/25.7 >/dev/null 2>&1
SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7
NVCC=$SDK/cuda/12.9/bin/nvcc
set -e
$NVCC -ccbin=g++ -std=c++17 -I"$NVBIT" -maxrregcount=24 -Xptxas -astoolspatch \
  --keep-device-functions -arch=sm_90 -Xcompiler -fPIC -c "$PROBE/inject.cu" -o "$WORK/inject.o"
$NVCC -ccbin=g++ -std=c++17 -I"$NVBIT" -dc -Xptxas -cloning=no -arch=sm_90 -O3 \
  -Xcompiler -fPIC -c "$PROBE/tool.cu" -o "$WORK/tool.o"
$NVCC -ccbin=g++ -arch=sm_90 -O3 "$WORK/tool.o" "$WORK/inject.o" -L"$NVBIT" -lnvbit \
  -lcuda -lcudart_static -shared -o "$WORK/wedge.so"
set +e
echo "=== built $WORK/wedge.so"

# ---- the workload: case 04's putget, built the way case 07 builds it --------
comshu_load_runtime
APP="$HOME/nvshmem_putget-li-$(sha1sum "$COMSHU_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" | cut -c1-10)"
if [ ! -x "$APP" ]; then
  comshu_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -lineinfo -rdc=true \
    -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets \
    "$COMSHU_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart
  comshu_load_runtime
fi
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$LD_LIBRARY_PATH"
export NVSHMEM_REMOTE_TRANSPORT=ibrc NVSHMEM_IB_ENABLE_IBGDA=0 NVSHMEM_DISABLE_P2P=1
# NVBit needs the SDK's nvdisasm (see the du04 note in gpu-backends/nvbit).
export PATH="$SDK/cuda/12.9/bin:$PATH"

arm() {  # arm <label> <mode|bare> [extra env...]
  local label=$1 mode=$2; shift 2
  local pre=""
  [ "$mode" != bare ] && pre="LD_PRELOAD=$WORK/wedge.so"
  ( export WEDGE_MODE=$mode "$@"
    timeout 120 mpirun -np 4 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
      -x NVSHMEM_BOOTSTRAP=MPI -x NVSHMEM_REMOTE_TRANSPORT -x NVSHMEM_IB_ENABLE_IBGDA \
      -x NVSHMEM_DISABLE_P2P -x WEDGE_MODE -x WEDGE_OFF -x WEDGE_DUMP -x WEDGE_KERNEL \
      -x WEDGE_FN ${pre:+-x $pre} "$APP" 65536 3 > "$WORK/$label.log" 2>&1 )
  local rc=$?
  local verdict=completed; [ $rc -eq 124 ] && verdict=HUNG; [ $rc -ne 0 ] && [ $rc -ne 124 ] && verdict="failed rc=$rc"
  printf '=== arm %-16s %-10s %s | %s\n' "$label" "$mode" "$verdict" \
    "$(grep -ah '\[wedge\] done' "$WORK/$label.log" | head -1)"
}

# First, confirm the instruction: list every LDL in the function, and check the
# bisect's offset names an LDL.64 in THIS build of NVSHMEM.
arm dump none WEDGE_DUMP=1
grep -ah "\[wedge\] .*nvshmemi_transfer_rma_p" "$WORK/dump.log" | sort -u | head -40
OFF=${WEDGE_OFF:-19680}

arm bare           bare
arm none_one       none    WEDGE_OFF=$OFF
arm empty_one      empty   WEDGE_OFF=$OFF
arm atomic_one     atomic  WEDGE_OFF=$OFF
arm channel_one    channel WEDGE_OFF=$OFF
arm channel_allldl channel
echo "=== logs in $WORK"
