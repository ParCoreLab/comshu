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
#SBATCH --cpus-per-task=20
#SBATCH --exclusive
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

# AFTER the build block, not before it — the same line, for the same reason,
# that case 04 documents at length. NVSHMEM_HOME globs to the NEWEST SDK (26.5
# on MN5) while pcomm_load_build/pcomm_load_runtime `module load
# nvidia-hpc-sdk/25.7`, and each of those PREPENDS 25.7's lib dir. Exported
# before the build block, 26.5 goes first, the build block shoves 25.7 in front
# of it, and the loader hands the binary a libnvshmem_host it was not linked
# against: "undefined symbol: nvshmem_selected_device_transport, version
# NVSHMEM", exit 127, no capture at all.
#
# This case had it at the top for its whole life and never paid, because it
# SKIPped on every host where the two SDKs differ. Giving it a host arm made the
# path live and it failed on the first run there (MN5 job 45463080), exactly as
# case 04's comment predicts.
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:${NCCL_HOME:+$NCCL_HOME/lib:}${LD_LIBRARY_PATH:-}"

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
    "$PCOMM" run -p ib,mlx5dv "$@" -o "$out" -- "$APP" "$ELEMS" "$ITERS"
}

# ib is selected alongside mlx5dv, and not for its own sake.
#
# The engine emits an EntityClose for every ibv_destroy_qp whenever ANY
# QP-recording provider is loaded, because from a void* it cannot tell a
# creation door it failed to hook from a queue pair belonging to a provider the
# user did not select (engine.cpp, pcomm_close_qp). On `-p mlx5dv` alone inside
# an MPI process that is 198 closes with opener 102 and no opens to match, and
# entity_close_accounted goes red on a capture that is actually correct — which
# is exactly the noise that assertion must not produce, since its whole value is
# being the cheapest detector of the NEXT missing door.
#
# Selecting ib removes the ambiguity at the source rather than suppressing the
# row: every verbs queue pair MPI and UCX create now has its open recorded, so an
# unmatched close means what it is supposed to mean again. It also turns this
# case's central claim into something asserted instead of assumed — under IBGDA
# there is no CPU data plane, so ib_post_send must be EMPTY while
# mlx5_device_send is not, and with the ibverbs provider loaded that is now a
# statement the capture can be checked against.

# ---- control run: hijack OFF ----------------------------------------------
mpi "$CTRL" > "$CTRL.app" 2>"$CTRL.err"; rc_ctrl=$?
if [ $rc_ctrl -ne 0 ]; then
  if grep -qE "nv_peer_mem, or nvidia_peermem detected|GPU-initiated communication|init failed for transport: IBGDA" \
       "$CTRL.err" 2>/dev/null; then
    skip "NVSHMEM declined IBGDA on this host (rc=$rc_ctrl)"
  fi
  fail "control run did not complete (rc=$rc_ctrl)"
fi

# ---- hijack run: hijack ON, bracketed by the NIC's own counters -----------
#
# THE ONLY ORACLE OUTSIDE PCOMM THIS CASE HAS. Everything else it checks is
# pcomm agreeing with pcomm: that no opcode was unknown, that no WQE was
# truncated, that the payload size derives from the workload, that the
# application's output is unchanged. All of those would still pass if the
# decoder invented plausible rows out of the right-shaped bytes, which is
# precisely the failure mode this provider has hit before and the reason
# mlx5_device_send carries a `decode` column at all.
#
# The NIC counts what it actually put on the wire, and it does not consult us.
# Snapshotting it around the instrumented run gives an independent upper bound.
IBCTR="/sys/class/infiniband/${PCOMM_HCA:-mlx5_0}/ports/1/counters"
wire_pkts_pre=$(cat "$IBCTR/port_xmit_packets" 2>/dev/null || echo "")
wire_data_pre=$(cat "$IBCTR/port_xmit_data" 2>/dev/null || echo "")

mpi "$OUT" "$DOORBELL_FLAG" > "$OUT.app" 2>"$OUT.err"; rc=$?

wire_pkts_post=$(cat "$IBCTR/port_xmit_packets" 2>/dev/null || echo "")
wire_data_post=$(cat "$IBCTR/port_xmit_data" 2>/dev/null || echo "")
[ $rc -eq 0 ] || fail "instrumented run did not complete (rc=$rc; 124 = timed out)"

# The host arm proves the platform before it swaps anything, and refuses on a
# host that can map the doorbell page into the GPU. If that refusal fires we
# chose the wrong arm, and the empty capture that follows would otherwise be
# asserted as a decode failure. Catch it here where the cause is still legible.
# Match the ONE branch that means "wrong arm". The guard declines three ways and
# all three say "NOT arming", so grepping that caught the benign one too: a probe
# made before the application had a CUDA context reports rc=201 and retries, and
# a run that went on to arm 4 UARs and decode 31 sends was still failed by this
# check (MN5 job 45463226). Only "DOES map" means the platform supports GPU
# submission and this arm was the wrong choice.
if [ "$ARM" = host ] && grep -q "DOES map the NIC doorbell page" "$OUT.err" 2>/dev/null; then
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

# ---- the wire check -------------------------------------------------------
#
# ONE-SIDED, and that is what makes it sound rather than merely plausible.
# Other traffic shares this HCA — MPI's bootstrap, UCX, the other rank — and
# every bit of it only ADDS to the counters. So "the NIC sent at least what we
# decoded" cannot be broken by noise, while its negation ("we decoded more than
# the NIC sent") has exactly one explanation: rows we made up. Under-reporting is
# a different failure and has its own detector, wqes_lapped.
#
# port_xmit_data counts 4-byte lanes, hence the x4, and it includes headers and
# ACKs, so wire bytes exceed payload bytes by construction. Rows that put no
# payload on the wire are excluded on the same predicate _sends uses: DUMP is
# ordering, and an undecodable opcode has length 0 because nothing was read, not
# because nothing was sent.
SENDCSV="$OUT.csv/mlx5_device_send.csv"
if [ -n "$wire_pkts_pre" ] && [ -n "$wire_pkts_post" ] && [ -f "$SENDCSV" ]; then
  d_pkts=$(( wire_pkts_post - wire_pkts_pre ))
  d_bytes=$(( (wire_data_post - wire_data_pre) * 4 ))
  read -r n_sends n_bytes <<<"$(awk -F, 'NR>1 && $NF!="unknown_op" && $6!="dump" \
      { n++; b += $11 } END { print n+0, b+0 }' "$SENDCSV")"
  echo "=== wire check: decoded ${n_sends} sends / ${n_bytes} B" \
       "vs NIC ${d_pkts} packets / ${d_bytes} B ==="
  if [ "$d_pkts" -le 0 ]; then
    echo "  (counters did not advance — not asserting; is $IBCTR the HCA in use?)"
  else
    [ "$n_sends" -le "$d_pkts" ] || \
      fail "decoded $n_sends sends but the NIC transmitted only $d_pkts packets"
    [ "$n_bytes" -le "$d_bytes" ] || \
      fail "decoded $n_bytes payload bytes but the NIC transmitted only $d_bytes"
  fi
else
  echo "=== wire check: SKIPPED (counters unreadable or no send rows) ==="
fi
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
