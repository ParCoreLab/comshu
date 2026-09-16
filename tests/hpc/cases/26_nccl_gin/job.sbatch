#!/bin/bash
# Case 26 — nccl_gin: NCCL's GPU-INITIATED data plane.
#
# Case 02 covers NCCL's ordinary traffic, which its proxy thread posts with
# ibv_post_send and the ibverbs provider records inside the call. GIN (NCCL
# 2.28+) does not use that door at all: a device kernel calls ncclGin::put, the
# GPU builds every WQE in a GPU-resident send queue, and the doorbell is rung by
# whoever the backend chose. On MN5 that is DOCA GPUNetIO's CPU proxy — a host
# thread — so --enable-host-doorbell is the arm, exactly as case 12 uses it for
# NVSHMEM's CPU-assisted IBGDA. Same decoder, same records, a different library.
#
#SBATCH --job-name=pcomm-26-nccl-gin
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/26_nccl_gin"
OUT=$(pcomm_out nccl_gin)                 # the armed capture (asserted)
CTRL="${OUT}_ctrl"                        # the passive control
mkdir -p "$(dirname "$OUT")"

skip() {
  echo "=== 26_nccl_gin : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"26_nccl_gin","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}
fail() {
  echo "=== 26_nccl_gin : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"26_nccl_gin","ok":false,"reason":"%s","checks":0}\n' "$1" > "$OUT.csv/result.json"
  exit 1
}

[ -d /sys/class/infiniband ] || skip "no InfiniBand device on this host"

pcomm_load_runtime
MPIRUN="${PCOMM_MPIRUN:-mpirun}"
command -v "$MPIRUN" >/dev/null || fail "no mpirun on PATH"

# ---- does this host's NCCL have the device API at all? ----------------------
#
# GIN arrived in NCCL 2.28, together with nccl_device.h and ncclDevCommCreate.
# Where NCCL_HOME resolves to something older the workload cannot be compiled,
# let alone run — and that is a host fact like "no InfiniBand device", so it
# belongs in skip() and not in a build failure. du04 is exactly that host: its
# SDK resolves to 2.27.7 (the 2.28.9 in /usr is not what NCCL_HOME finds), and
# without this the case would report FAIL for owning an older library than it
# needs, on a host where it simply cannot run.
[ -n "${NCCL_HOME:-}" ] && [ -d "$NCCL_HOME" ] || skip "NCCL_HOME did not resolve"
[ -r "$NCCL_HOME/include/nccl_device.h" ] || \
  skip "NCCL at $NCCL_HOME has no device API (no nccl_device.h) — GIN needs 2.28+"
nccl_ver=$(awk '/^#define NCCL_MAJOR /{maj=$3} /^#define NCCL_MINOR /{min=$3} END{print maj"."min}' \
           "$NCCL_HOME/include/nccl.h" 2>/dev/null)
case "$nccl_ver" in
  2.2[89]|2.[3-9][0-9]|[3-9].*) : ;;
  *) skip "NCCL $nccl_ver has no GIN — needs 2.28+" ;;
esac
echo "=== 26_nccl_gin : NCCL $nccl_ver at $NCCL_HOME ==="

# ---- workload ---------------------------------------------------------------
# Named for its source, like the putget binaries, so an edit cannot be run as if
# it were the old build — the failure that let case 12 pass for three weeks on a
# kernel that never ran.
APP="${PCOMM_GIN_APP:-$HOME/gin_putring-$(sha1sum "$CASE_DIR/gin_putring.cu" | cut -c1-10)}"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  command -v "${PCOMM_NVCC:-nvcc}" >/dev/null 2>&1 || fail "no nvcc (PCOMM_SDK=${PCOMM_SDK:-unset})"
  ${PCOMM_NVCC:-nvcc} -ccbin=mpic++ -std=c++17 -O2 --expt-extended-lambda \
    -gencode arch=compute_80,code=sm_80 -gencode arch=compute_90,code=sm_90 \
    -I"$NCCL_HOME/include" "$CASE_DIR/gin_putring.cu" -o "$APP" \
    -L"$NCCL_HOME/lib" -lnccl -lcudart -Xlinker -rpath -Xlinker "$NCCL_HOME/lib" \
    || fail "workload did not build (needs NCCL >= 2.28 for the device API)"
  pcomm_load_runtime
fi

# AFTER the build block, for the reason case 04 documents at length: NCCL_HOME
# globs to the newest SDK while pcomm_load_build/pcomm_load_runtime `module load`
# an older one and PREPEND its lib dir. Exported first, the loader hands the
# binary a libnccl it was not linked against.
export LD_LIBRARY_PATH="$NCCL_HOME/lib:${LD_LIBRARY_PATH:-}"

# ---- force GIN, and force which GIN -----------------------------------------
#
# NCCL_NET_PLUGIN=none: GIN's backends live in NCCL's INTERNAL net_ib transport.
# MN5 loads HPC-X's external network plugin by default, which has none, and the
# communicator then reports ginType=0 with deviceApiSupport=0 — measured, job
# 45900900, where every variant died before sending anything.
#
# And NOT NCCL_P2P_DISABLE, which case 12 sets for NVSHMEM: the device API needs
# symmetric memory, symmetric memory needs CUDA P2P among local ranks, and
# disabling it is the other half of why that first job captured nothing.
export NCCL_NET_PLUGIN=none
export NCCL_GIN_TYPE=3                    # GIN_IB_GDAKI — the GPU-initiated backend
export NCCL_GIN_GDAKI_NIC_HANDLER=1       # CPU_PROXY: a host thread rings the doorbell
export NCCL_DEBUG=WARN

CHUNK=128 NPUT=1024 ITERS=20              # = meta.json's workload block
mpi() {  # mpi <out-dir> <extra pcomm args...>
  local out="$1"; shift
  rm -rf "$out" "$out.csv"; mkdir -p "$out"
  timeout 420 "$MPIRUN" -np 2 --map-by ppr:1:node ${PCOMM_MPIRUN_EXTRA:-} \
    -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH \
    -x NCCL_NET_PLUGIN -x NCCL_GIN_TYPE -x NCCL_GIN_GDAKI_NIC_HANDLER -x NCCL_DEBUG \
    "$PCOMM" run -p ib,mlx5dv "$@" -o "$out" -- "$APP" "$CHUNK" "$NPUT" "$ITERS"
}

# ---- control run: no door ---------------------------------------------------
# Also the platform probe: gin_putring exits 4, and only 4, when this
# communicator has no GIN — which is a host fact, not a capture failure.
mpi "$CTRL" > "$CTRL.app" 2>"$CTRL.err"; rc_ctrl=$?
if [ $rc_ctrl -eq 4 ] || grep -q "no GIN on this communicator\|no device communicator with GIN here" "$CTRL.err" 2>/dev/null; then
  skip "this NCCL/communicator has no GIN (deviceApiSupport or ginType is 0)"
fi
[ $rc_ctrl -eq 0 ] || fail "control run did not complete (rc=$rc_ctrl; 124 = timed out)"

# ---- armed run --------------------------------------------------------------
mpi "$OUT" --enable-host-doorbell > "$OUT.app" 2>"$OUT.err"; rc=$?
[ $rc -eq 0 ] || fail "instrumented run did not complete (rc=$rc; 124 = timed out)"

# The host arm proves the platform before it swaps anything and declines where a
# UAR can be mapped into the GPU. If that fires, this host wants the GPU arm and
# the empty capture that follows would read like a decode failure. Catch it here.
if grep -q "DOES map the NIC doorbell page" "$OUT.err" 2>/dev/null; then
  skip "this platform maps the NIC doorbell into the GPU; GDAKI's plane needs the GPU arm here (things-to-circle-back-to.md)"
fi

# ---- negative control -------------------------------------------------------
# The door's streams must be ABSENT from the passive run: their presence would
# mean the arm leaked into a run that did not ask for it.
ctrl_leak=""
for id in 212 214; do
  for f in "$CTRL"/rank-*/"$id".pcs; do
    [ -e "$f" ] && [ -s "$f" ] && ctrl_leak="$ctrl_leak $f"
  done
done
[ -z "$ctrl_leak" ] || fail "doorbell streams present in the passive control:$ctrl_leak"

# The workload checks every byte it receives, so its own line is an oracle: it
# must be identical with and without the door, and it must be there at all.
app_ctrl="$(grep -h 'gin put-ring OK' "$CTRL.app" | sort)"
app_arm="$(grep -h 'gin put-ring OK' "$OUT.app" | sort)"
[ -n "$app_arm" ] || fail "armed run produced no 'gin put-ring OK' line"
[ "$app_ctrl" = "$app_arm" ] || fail "application output differs under the door"

# ---- assert -----------------------------------------------------------------
echo "=== captured streams (armed) ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"

# What the door itself says it did. Not an assertion — gin_puts_arrived is the
# assertion — but the first line to read when that one fails.
grep -h "host doorbell: uars_armed" "$OUT.err" | sed 's/^/=== /' || true

pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
