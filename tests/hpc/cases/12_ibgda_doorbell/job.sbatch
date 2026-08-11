#!/bin/bash
# Case 12 — ibgda_doorbell: the DATA plane. Case 09 proves the mlx5dv provider
# captures IBGDA's control plane (queue pairs, topology); this proves the
# doorbell hijack captures the sends themselves — the GPU-initiated traffic that
# has no CPU data plane to hook. It runs the workload twice: once with the hijack
# (the capture under test) and once without (a negative control), and asserts
# both that the hijack saw real sends and that a passive run saw none.
#
# Like case 09 this needs IBGDA and self-skips where it cannot run.
#
#SBATCH --job-name=pcomm-12-ibgda-doorbell
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=2
#SBATCH --gres=gpu:2
#SBATCH --time=00:10:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/12_ibgda_doorbell"
OUT=$(pcomm_out ibgda_doorbell)          # the hijacked capture (asserted)
CTRL="${OUT}_ctrl"                        # the passive control
# The .app/.err redirects below are opened by the shell before mpi() runs its
# own mkdir, so the parent directory has to exist first.
mkdir -p "$(dirname "$OUT")"

export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:${NCCL_HOME:+$NCCL_HOME/lib:}${LD_LIBRARY_PATH:-}"

skip() {
  echo "=== 12_ibgda_doorbell : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"12_ibgda_doorbell","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}
fail() {
  echo "=== 12_ibgda_doorbell : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"12_ibgda_doorbell","ok":false,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 1
}

# ---- prerequisites (identical to case 09) ---------------------------------
[ -d /sys/class/infiniband ] || skip "no InfiniBand device on this host"
have_peermem=0
lsmod 2>/dev/null | grep -qE "nvidia_peermem|nv_peer_mem" && have_peermem=1
grep -q "PeerMappingOverride=1" /proc/driver/nvidia/params 2>/dev/null || \
  skip "NVIDIA driver has no PeerMappingOverride=1 — IBGDA cannot map the UAR to the GPU"
[ "$have_peermem" = "1" ] || skip "no nvidia_peermem/nv_peer_mem module loaded"

MPIRUN="${PCOMM_MPIRUN:-mpirun}"
command -v "$MPIRUN" >/dev/null || fail "no mpirun on PATH"

# ---- workload (same binary as cases 04/06/07/09) --------------------------
APP="${PCOMM_IBGDA_APP:-$HOME/nvshmem_putget_sm80}"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  command -v "${PCOMM_NVCC:-nvcc}" >/dev/null 2>&1 || fail "no nvcc (PCOMM_SDK=${PCOMM_SDK:-unset})"
  [ -n "${NVSHMEM_HOME:-}" ] && [ -d "$NVSHMEM_HOME" ] || fail "NVSHMEM_HOME did not resolve"
  ${PCOMM_NVCC:-nvcc} -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch="${PCOMM_CUDA_ARCH:-compute_80}",code="${PCOMM_CUDA_CODE:-sm_80}" \
    -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart || fail "workload did not build"
  pcomm_load_runtime
fi

# ---- force GPU-initiated IB (identical to case 09) ------------------------
export NVSHMEM_BOOTSTRAP=MPI
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=gpu
export NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem

ELEMS=1024 ITERS=20
mpi() {  # mpi <out-dir> <extra pcomm args...>
  local out="$1"; shift
  rm -rf "$out" "$out.csv"; mkdir -p "$out"
  timeout 300 "$MPIRUN" -np 2 ${PCOMM_MPIRUN_EXTRA:-} \
    -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH \
    -x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P -x NVSHMEM_REMOTE_TRANSPORT \
    -x NVSHMEM_IB_ENABLE_IBGDA -x NVSHMEM_IBGDA_NIC_HANDLER \
    -x NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE \
    "$PCOMM" run -p mlx5dv "$@" -o "$out" -- "$APP" "$ELEMS" "$ITERS"
}

# ---- control run: hijack OFF ----------------------------------------------
mpi "$CTRL" > "$CTRL.app" 2>"$CTRL.err"; rc_ctrl=$?
if [ $rc_ctrl -ne 0 ]; then
  if grep -qiE "GPU-initiated communication|peermem|init failed for transport: IBGDA" \
       "$CTRL.err" 2>/dev/null; then
    skip "NVSHMEM declined IBGDA on this host (rc=$rc_ctrl)"
  fi
  fail "control run did not complete (rc=$rc_ctrl)"
fi

# ---- hijack run: hijack ON ------------------------------------------------
mpi "$OUT" --enable-doorbell-hijack > "$OUT.app" 2>"$OUT.err"; rc=$?
[ $rc -eq 0 ] || fail "hijacked run did not complete (rc=$rc; 124 = timed out)"

# ---- negative control -----------------------------------------------------
# A passive run must produce NO doorbell/send streams: those ids (212/214) are
# emitted only by the hijack, so their presence in $CTRL would mean the hijack
# leaked into a run that did not ask for it.
ctrl_leak=""
for id in 212 214; do
  for f in "$CTRL"/rank-*/"$id".pcs; do
    [ -e "$f" ] && [ -s "$f" ] && ctrl_leak="$ctrl_leak $f"
  done
done
[ -z "$ctrl_leak" ] || fail "hijack streams present in the passive control:$ctrl_leak"

# The application's own output must be byte-identical with and without the
# hijack — the man-in-the-middle must not change what the workload computes.
app_ctrl="$(grep -h 'put-ring' "$CTRL.app" | sort)"
app_hij="$(grep -h 'put-ring' "$OUT.app" | sort)"
[ -n "$app_hij" ] || fail "hijacked run produced no 'put-ring' output"
[ "$app_ctrl" = "$app_hij" ] || fail "application output differs under the hijack"

# ---- assert the hijacked capture ------------------------------------------
echo "=== captured streams (hijack) ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
