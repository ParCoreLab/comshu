#!/bin/bash
# Case 28 — nccl_lsa: stores through NCCL's symmetric windows, over NVLink.
#
# Case 05 is NCCL's classic transport, whose peer buffers are exchanged one
# connection at a time and which comshu cannot place in an owner's buffer
# (plans/PLAN-20260817-0705-...: no identity survives the fd exchange). NCCL's
# SYMMETRIC windows (2.27+) are a different shape: every local rank's window is
# mapped into one flat range at a fixed stride, so an offset names the same
# byte on every rank — the NVSHMEM shape. This case asks whether comshu places
# those stores in the owner's window, exactly, the way it places NVSHMEM's.
#
#SBATCH --job-name=comshu-28-nccl-lsa
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:20:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$COMSHU_ROOT/tests/hpc/cases/28_nccl_lsa"
OUT=$(comshu_out nccl_lsa)
mkdir -p "$(dirname "$OUT")"

skip() {
  echo "=== 28_nccl_lsa : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"28_nccl_lsa","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}
fail() {
  echo "=== 28_nccl_lsa : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"28_nccl_lsa","ok":false,"reason":"%s","checks":0}\n' "$1" > "$OUT.csv/result.json"
  exit 1
}

comshu_load_runtime
MPIRUN="${COMSHU_MPIRUN:-mpirun}"
command -v "$MPIRUN" >/dev/null || fail "no mpirun on PATH"

# Symmetric windows and ncclGetLsaPointer arrived with the device API (2.28).
[ -n "${NCCL_HOME:-}" ] && [ -r "$NCCL_HOME/include/nccl_device.h" ] || \
  skip "NCCL at ${NCCL_HOME:-unresolved} has no device API (needs 2.28+)"
nccl_ver=$(awk '/^#define NCCL_MAJOR /{maj=$3} /^#define NCCL_MINOR /{min=$3} END{print maj"."min}' \
           "$NCCL_HOME/include/nccl.h" 2>/dev/null)

# Named for its source and NCCL version, like gin_putring: an edit cannot run
# as if it were the old build.
APP="${COMSHU_LSA_APP:-$HOME/lsa_ring-$(sha1sum "$CASE_DIR/lsa_ring.cu" | cut -c1-10)-nccl$nccl_ver}"
if [ ! -x "$APP" ]; then
  comshu_load_build
  command -v "${COMSHU_NVCC:-nvcc}" >/dev/null 2>&1 || fail "no nvcc (COMSHU_SDK=${COMSHU_SDK:-unset})"
  ${COMSHU_NVCC:-nvcc} -ccbin=mpic++ -std=c++17 -O2 -lineinfo \
    -gencode arch=compute_90,code=sm_90 \
    -I"$NCCL_HOME/include" "$CASE_DIR/lsa_ring.cu" -o "$APP" \
    -L"$NCCL_HOME/lib" -lnccl -lcudart -Xlinker -rpath -Xlinker "$NCCL_HOME/lib" \
    || fail "workload did not build"
  comshu_load_runtime
fi
# After the build block — see case 26 for why the order matters.
export LD_LIBRARY_PATH="$NCCL_HOME/lib:${LD_LIBRARY_PATH:-}"

# Symmetric memory needs CUDA P2P among the local ranks, so P2P stays on.
export NCCL_DEBUG=WARN NCCL_P2P_DISABLE=0

# p2p (gpumem) sees the stores; nccl records the windows and, through NCCL's
# public ncclGetPeerDevicePointer, where each peer's copy is mapped here. The
# reader needs both to place a store in its owner's window.
BYTES=131072 ITERS=10                     # = meta.json's workload block
DEADLINE=900  # NVBit's fixed cost of processing NCCL's module (case 05: ~435 s)
rm -rf "$OUT" "$OUT.csv"; mkdir -p "$OUT"
timeout "$DEADLINE" "$MPIRUN" -np 4 --map-by ppr:4:node \
  -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH -x NCCL_DEBUG -x NCCL_P2P_DISABLE \
  "$COMSHU" run -p p2p,nccl -o "$OUT" -- "$APP" "$BYTES" "$ITERS" > "$OUT.app" 2>"$OUT.err"
rc=$?
cat "$OUT.app"; tail -50 "$OUT.err"
if [ $rc -eq 4 ] || grep -q "no symmetric" "$OUT.err" 2>/dev/null; then
  skip "this communicator has no symmetric memory across its ranks"
fi
[ $rc -eq 0 ] || fail "instrumented run did not complete (rc=$rc; 124 = exceeded ${DEADLINE}s)"
grep -q "lsa ring OK" "$OUT.app" || fail "workload printed no 'lsa ring OK' line"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
comshu_derive "$OUT"
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
