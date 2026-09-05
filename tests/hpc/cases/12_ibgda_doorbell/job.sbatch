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

# ---- which doorbell mechanism can this host use? --------------------------
#
# This used to SKIP whenever the driver had no PeerMappingOverride=1, which made
# the case permanently dead on MN5 — the cluster every other HPC case runs on.
# That was right when the GPU hijack was the only mechanism: without a BAR page
# mapped into the GPU there is no GPU doorbell store to intercept.
#
# It is no longer the only mechanism. Where the driver refuses that mapping,
# NVSHMEM runs CPU-assisted IBGDA — the GPU still builds every WQE in a
# GPU-resident send queue and advances a producer index, and a HOST thread rings
# the doorbell — and --enable-host-doorbell watches that instead. Same decoder,
# same records, same assertions; only the writer differs.
#
# So the prerequisite check now SELECTS an arm rather than giving up, and skips
# only where neither can work. Measured 2026-09-05: du04 takes the gpu arm
# (job on a100), MN5 the host arm (job 45449988, 7,232 sends decoded). See
# tests/hpc/probes/uar_doorbell_mode/ for the probe that answers this in a
# minute on any new cluster.
have_peermem=0
lsmod 2>/dev/null | grep -qE "nvidia_peermem|nv_peer_mem" && have_peermem=1
if grep -q "PeerMappingOverride=1" /proc/driver/nvidia/params 2>/dev/null && \
   [ "$have_peermem" = "1" ]; then
  ARM=gpu
  HANDLER=gpu
  DOORBELL_FLAG=--enable-doorbell-hijack
else
  ARM=host
  HANDLER=cpu
  DOORBELL_FLAG=--enable-host-doorbell
fi
echo "=== 12_ibgda_doorbell : arm=$ARM (NIC handler $HANDLER, $DOORBELL_FLAG) ==="

# Load the runtime BEFORE looking for a launcher, the way case 04 does. This
# case only ever called pcomm_load_runtime inside the app-build branch below,
# which runs after this check — so on any host where mpirun needs a module it
# failed with "no mpirun on PATH" before it could load the module that provides
# one. That ordering bug was invisible for as long as this case SKIPped on every
# such host, and surfaced the moment the host arm let it actually run (MN5 job
# 45462870).
pcomm_load_runtime
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
export NVSHMEM_IBGDA_NIC_HANDLER="$HANDLER"
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
mpi "$OUT" "$DOORBELL_FLAG" > "$OUT.app" 2>"$OUT.err"; rc=$?
[ $rc -eq 0 ] || fail "instrumented run did not complete (rc=$rc; 124 = timed out)"

# The host arm proves the platform before it swaps anything, and refuses on a
# host that can map the doorbell page into the GPU. If that refusal fires we
# chose the wrong arm, and the empty capture that follows would otherwise be
# asserted as a decode failure. Catch it here where the cause is still legible.
if [ "$ARM" = host ] && grep -q "NOT arming" "$OUT.err" 2>/dev/null; then
  fail "host arm refused to arm — this host supports GPU submission, so arm=gpu was correct"
fi

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
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
