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
#SBATCH --job-name=comshu-26-nccl-gin
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$COMSHU_ROOT/tests/hpc/cases/26_nccl_gin"
OUT=$(comshu_out nccl_gin)                 # the armed capture (asserted)
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

comshu_load_runtime
MPIRUN="${COMSHU_MPIRUN:-mpirun}"
command -v "$MPIRUN" >/dev/null || fail "no mpirun on PATH"

# ---- which NCCL: the first one with the device API ------------------------
#
# GIN arrived in NCCL 2.28, together with nccl_device.h and ncclDevCommCreate.
# NCCL_HOME globs to the HPC SDK's NCCL, which on du04 is 2.27.7, too old, while
# the distro's 2.28.9 sits in /usr. That one cannot simply be named as
# NCCL_HOME: it is not laid out as include/ + lib/, and -I/usr/include cannot
# beat nvcc's implicit include of the SDK's own nccl.h, because a system
# directory keeps its system position. So a usable system NCCL is staged as a
# private NCCL_HOME (symlinks only) and that directory goes first. The case
# still skips only when no NCCL on the host has the device API.
nccl_has_device_api() { [ -r "$1/include/nccl_device.h" ]; }
if ! { [ -n "${NCCL_HOME:-}" ] && nccl_has_device_api "$NCCL_HOME"; }; then
  sdk_nccl="${NCCL_HOME:-unresolved}"
  for _lib in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
    if [ -r /usr/include/nccl_device.h ] && [ -e "$_lib/libnccl.so.2" ]; then
      stage="$(dirname "$OUT")/.nccl-system"
      rm -rf "$stage"; mkdir -p "$stage/include" "$stage/lib"
      ln -s /usr/include/nccl*.h /usr/include/nccl_device "$stage/include/" 2>/dev/null
      ln -s "$_lib"/libnccl.so* "$stage/lib/"
      export NCCL_HOME="$stage"
      echo "=== 26_nccl_gin : $sdk_nccl has no device API; using the system NCCL ($_lib) ==="
      break
    fi
  done
  unset _lib
fi
[ -n "${NCCL_HOME:-}" ] && [ -d "$NCCL_HOME" ] || skip "NCCL_HOME did not resolve"
nccl_has_device_api "$NCCL_HOME" || \
  skip "no NCCL on this host has the device API (no nccl_device.h) — GIN needs 2.28+"
nccl_ver=$(awk '/^#define NCCL_MAJOR /{maj=$3} /^#define NCCL_MINOR /{min=$3} END{print maj"."min}' \
           "$NCCL_HOME/include/nccl.h" 2>/dev/null)
case "$nccl_ver" in
  2.2[89]|2.[3-9][0-9]|[3-9].*) : ;;
  *) skip "NCCL $nccl_ver has no GIN — needs 2.28+" ;;
esac

# ---- workload ---------------------------------------------------------------
# Named for its source, like the putget binaries, so an edit cannot be run as if
# it were the old build — the failure that let case 12 pass for three weeks on a
# kernel that never ran.
# The NCCL version is in the name too: the binary is linked against one, and du04
# and MN5 build against different ones.
APP="${COMSHU_GIN_APP:-$HOME/gin_putring-$(sha1sum "$CASE_DIR/gin_putring.cu" | cut -c1-10)-nccl$nccl_ver}"
if [ ! -x "$APP" ]; then
  comshu_load_build
  command -v "${COMSHU_NVCC:-nvcc}" >/dev/null 2>&1 || fail "no nvcc (COMSHU_SDK=${COMSHU_SDK:-unset})"
  ${COMSHU_NVCC:-nvcc} -ccbin=mpic++ -std=c++17 -O2 --expt-extended-lambda \
    -gencode arch=compute_80,code=sm_80 -gencode arch=compute_90,code=sm_90 \
    -I"$NCCL_HOME/include" "$CASE_DIR/gin_putring.cu" -o "$APP" \
    -L"$NCCL_HOME/lib" -lnccl -lcudart -Xlinker -rpath -Xlinker "$NCCL_HOME/lib" \
    || fail "workload did not build (needs NCCL >= 2.28 for the device API)"
  comshu_load_runtime
fi

# AFTER the build block, for the reason case 04 documents at length: NCCL_HOME
# globs to the newest SDK while comshu_load_build/comshu_load_runtime `module load`
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
export NCCL_DEBUG=WARN

# ---- which doorbell writer, and so which arm --------------------------------
#
# Where the driver lets the GPU map the NIC's doorbell page (PeerMappingOverride=1,
# du04), GDAKI's own default is the GPU writing it, and the arm is the doorbell
# hijack: it swaps the page for a watched one at cuMemHostGetDevicePointer.
# NCCL reaches that call only through cudaGetDriverEntryPoint pointers, so this
# arm captured nothing until libcuda's cells were patched (bd895fc). Where the
# driver refuses (MN5), the writer is a host thread and the arm is
# --enable-host-doorbell. Both arms feed the same decoder and the same records,
# so every assertion below holds for either.
if grep -q "PeerMappingOverride=1" /proc/driver/nvidia/params 2>/dev/null; then
  export NCCL_GIN_GDAKI_NIC_HANDLER=2     # GPU_SM_DB: the GPU rings the doorbell
  ARM=--enable-doorbell-hijack
else
  export NCCL_GIN_GDAKI_NIC_HANDLER=1     # CPU_PROXY: a host thread rings the doorbell
  ARM=--enable-host-doorbell
fi
echo "=== 26_nccl_gin : NCCL $nccl_ver at $NCCL_HOME, NIC handler $NCCL_GIN_GDAKI_NIC_HANDLER, arm $ARM ==="

# One rank per node where there are two nodes (MN5, the network path); both on
# the one node otherwise (du04, which has two A100s and one box).
# The oracle follows: meta.json declares MN5's two nodes, and a one-node run
# says so beside its results (sql_check.sh reads workload_override.json).
if [ "${SLURM_JOB_NUM_NODES:-1}" -ge 2 ]; then MAPBY=ppr:1:node; NODES=2; else MAPBY=ppr:2:node; NODES=1; fi

CHUNK=128 NPUT=1024 ITERS=20              # = meta.json's workload block
# Overridable so a probe can run this exact case with more providers beside the
# two it asserts on — tests/hpc/probes/kernel_attr adds cupti for kernel windows.
PROVIDERS="${COMSHU_GIN_PROVIDERS:-ib,mlx5dv}"
mpi() {  # mpi <out-dir> <extra comshu args...>
  local out="$1"; shift
  rm -rf "$out" "$out.csv"; mkdir -p "$out"
  timeout 420 "$MPIRUN" -np 2 --map-by "$MAPBY" ${COMSHU_MPIRUN_EXTRA:-} \
    -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
    -x NCCL_NET_PLUGIN -x NCCL_GIN_TYPE -x NCCL_GIN_GDAKI_NIC_HANDLER -x NCCL_DEBUG \
    "$COMSHU" run -p "$PROVIDERS" "$@" -o "$out" -- "$APP" "$CHUNK" "$NPUT" "$ITERS"
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
mpi "$OUT" "$ARM" > "$OUT.app" 2>"$OUT.err"; rc=$?
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
echo "=== captured streams (armed) ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"

# What the door itself says it did. Not an assertion — gin_puts_arrived is the
# assertion — but the first line to read when that one fails.
grep -hE "host doorbell: uars_armed|doorbell hijack: uars_hijacked" "$OUT.err" | sed 's/^/=== /' || true

[ "$NODES" -eq 2 ] || printf '{"nodes": %d}\n' "$NODES" > "$OUT.csv/workload_override.json"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
