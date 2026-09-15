#!/bin/bash
# Case 24 — cuda_memcpy_ring: peer copies as RANK-SPACE edges.
#
# Case 23 proves the record; this proves the resolution. There, one process
# drives four GPUs, so every copy edge it can produce is rank 0 -> rank 0 and
# the chain that turns a device ordinal into an owning rank
# (_node -> _device_key -> _device_owner) is never exercised. Here four
# processes each own one GPU and cudaMemcpyPeer round a CUDA-IPC ring, so the
# whole matrix is a cross-rank claim — and the entire inter-rank traffic is a
# copy engine, which no other case in the suite can say.
#
# Runs without a scheduler as well as under one; the #SBATCH lines are comments
# to a plain bash run.
#
#SBATCH --job-name=pcomm-24-cuda-memcpy-ring
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:10:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/24_cuda_memcpy_ring"
OUT=$(pcomm_out cuda_memcpy_ring)

# The same numbers meta.json's `workload` block declares; ipcring prints its own
# arithmetic so a drift between the two shows up in the log.
RANKS="${PCOMM_RING_RANKS:-4}"
ITERS="${PCOMM_RING_ITERS:-100}"
MSG_BYTES="${PCOMM_RING_BYTES:-1048576}"

skip() {
  echo "=== 24_cuda_memcpy_ring : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"24_cuda_memcpy_ring","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}
fail() {
  echo "=== 24_cuda_memcpy_ring : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"24_cuda_memcpy_ring","ok":false,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 1
}

pcomm_load_runtime
command -v nvidia-smi >/dev/null 2>&1 || skip "no NVIDIA driver on this host"
have_gpus=$(nvidia-smi --query-gpu=index --format=csv,noheader 2>/dev/null | wc -l)
[ "${have_gpus:-0}" -ge "$RANKS" ] || skip "case needs $RANKS GPUs, host has ${have_gpus:-0}"
MPIRUN="${PCOMM_MPIRUN:-mpirun}"
command -v "$MPIRUN" >/dev/null || skip "no mpirun on PATH"

cupti_so=""
for d in $(echo "$PCOMM_PROVIDER_PATH" | tr ':' ' '); do
  [ -e "$d/libcupti_trace.so" ] && cupti_so="$d/libcupti_trace.so"
done
[ -n "$cupti_so" ] || skip "this build has no libcupti_trace.so (configure with -DPCOMM_CUPTI=ON)"

APP="${PCOMM_IPCRING_APP:-$OUT.ipcring}"
mkdir -p "$(dirname "$APP")"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  command -v "${PCOMM_NVCC:-nvcc}" >/dev/null 2>&1 || fail "no nvcc (PCOMM_SDK=${PCOMM_SDK:-unset})"
  ${PCOMM_NVCC:-nvcc} -O2 -ccbin=mpic++ \
    -gencode arch=compute_80,code=sm_80 \
    -gencode arch=compute_90,code=sm_90 \
    -Wno-deprecated-gpu-targets \
    "$CASE_DIR/ipcring.cu" -o "$APP" -lcudart || fail "workload did not build"
  pcomm_load_runtime
fi

# Bare first, then traced, in the same allocation. A capture of a workload that
# did not run is indistinguishable from a tracer that captured nothing, and the
# bare run also prints the oracle beside the captured numbers.
echo "=== bare run (no tracer) ==="
timeout 300 "$MPIRUN" -np "$RANKS" ${PCOMM_MPIRUN_EXTRA:-} --oversubscribe \
  -x PATH -x LD_LIBRARY_PATH "$APP" "$ITERS" "$MSG_BYTES"
bare_rc=$?
[ $bare_rc -eq 0 ] || fail "workload does not run on this host without pcomm (rc=$bare_rc)"

rm -rf "$OUT" "$OUT.csv" "$OUT.pq"; mkdir -p "$OUT"

# NO CUDA_VISIBLE_DEVICES PINNING, and it is a requirement rather than an
# omission: cudaMemcpyPeer takes the PEER's ordinal and it has to be meaningful
# in the calling process, so every rank must see every GPU. That is also the
# configuration in which _device_owner has real work to do — four ranks each
# claiming one of four visible devices — which is the point of the case.
echo "=== traced run ==="
timeout 300 "$MPIRUN" -np "$RANKS" ${PCOMM_MPIRUN_EXTRA:-} --oversubscribe \
  -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH \
  "$PCOMM" run -p cupti -o "$OUT" -- "$APP" "$ITERS" "$MSG_BYTES"
rc=$?; echo "traced exit=$rc"
if [ $rc -ne 0 ]; then
  echo "FAILED: traced run did not complete (rc=$rc; 124 = timed out)."
  mkdir -p "$OUT.csv"
  printf '{"case":"24_cuda_memcpy_ring","ok":false,"checks":0,"rc":%d}\n' "$rc" > "$OUT.csv/result.json"
  exit 1
fi

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
