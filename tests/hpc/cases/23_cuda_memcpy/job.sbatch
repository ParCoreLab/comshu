#!/bin/bash
# Case 23 — cuda_memcpy: the only case whose entire communication is a
# HOST-INITIATED COPY.
#
# Every other case's traffic is an instruction a kernel executed (gpumem), a
# work request on a queue pair (ib/ibgda), or a library call (ucx/nccl). A
# copy-engine transfer is none of those — it retires no instruction and posts no
# work request — and pcomm/run.cpp's gpumem registry row has said so in a
# comment for as long as the provider has existed. cudaMemcpyPeer between two
# GPUs is nevertheless communication, so before cuda_memcpy (534) a capture of
# this workload was an empty communication matrix with a clean bill of health.
#
# Runs WITHOUT a scheduler as well as under one: the #SBATCH lines below are
# comments to a plain bash run, the property suite/all_in_one.sbatch already
# relies on. It matters here because the machine this case is written for (du04,
# 4x A100 with NVLink between every pair) has no SLURM at all.
#
#SBATCH --job-name=pcomm-23-cuda-memcpy
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --gres=gpu:4
#SBATCH --time=00:10:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/23_cuda_memcpy"
OUT=$(pcomm_out cuda_memcpy)

# The workload's parameters, and they are the SAME NUMBERS meta.json's `workload`
# block declares. Kept here as variables and passed on the command line so the
# two cannot drift silently: copybench prints its own arithmetic at the end of
# every run, so a mismatch between this file and meta.json shows up in the log
# beside the assertion that used the other value.
# 200 iterations = 4400 copies, and the number is chosen rather than round.
# Recorder::Config::buffer_capacity is 4096, and GpuDevice is emitted from the
# ARENA-FLUSH path — Tracer's own comment states the boundary: "a capture too
# short to fill a single buffer never flushes in-run and will carry no
# gpu_device rows". Measured here, not assumed: the first run of this case used
# 4 iterations, produced 88 copies, and came back with no gpu_device stream at
# all, so `_device_key` fell back to its 'O' ordinal keys. That fallback is
# correct and the case would still have passed; it would just never have
# exercised the (node_hash, pci_bus) path that every multi-rank capture uses.
ITERS="${PCOMM_COPY_ITERS:-200}"
MSG_BYTES="${PCOMM_COPY_BYTES:-4194304}"
GPUS="${PCOMM_COPY_GPUS:-4}"

# SKIP means one thing only: this host cannot run the case, so it has nothing to
# say. It is NOT a place to put "something went wrong" — see case 09, where
# exactly that confusion produced a green board that had built nothing.
skip() {
  echo "=== 23_cuda_memcpy : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"23_cuda_memcpy","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}
fail() {
  echo "=== 23_cuda_memcpy : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"23_cuda_memcpy","ok":false,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 1
}

# ---- prerequisites --------------------------------------------------------
pcomm_load_runtime
command -v nvidia-smi >/dev/null 2>&1 || skip "no NVIDIA driver on this host"
have_gpus=$(nvidia-smi --query-gpu=index --format=csv,noheader 2>/dev/null | wc -l)
[ "${have_gpus:-0}" -ge "$GPUS" ] || \
  skip "case needs $GPUS GPUs, host has ${have_gpus:-0}"

# The backend, not a provider directory: libcupti_trace.so builds under
# gpu-backends/ because it captures no transport. common.sh searches both, and
# the check is for the LIBRARY rather than for the CUDA toolkit — a build
# configured without -DPCOMM_CUPTI has no copy capture at all and this case would
# otherwise report an empty table as a defect in the tracer.
cupti_so=""
for d in $(echo "$PCOMM_PROVIDER_PATH" | tr ':' ' '); do
  [ -e "$d/libcupti_trace.so" ] && cupti_so="$d/libcupti_trace.so"
done
[ -n "$cupti_so" ] || skip "this build has no libcupti_trace.so (configure with -DPCOMM_CUPTI=ON)"

# ---- workload -------------------------------------------------------------
APP="${PCOMM_COPYBENCH_APP:-$OUT.copybench}"
mkdir -p "$(dirname "$APP")"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  command -v "${PCOMM_NVCC:-nvcc}" >/dev/null 2>&1 || \
    fail "no nvcc (PCOMM_SDK=${PCOMM_SDK:-unset})"
  # sm_80 covers the A100 this case is written for; sm_90 an H100. Both are
  # emitted so the same binary runs on either without the case having to know
  # which machine it is on.
  ${PCOMM_NVCC:-nvcc} -O2 \
    -gencode arch=compute_80,code=sm_80 \
    -gencode arch=compute_90,code=sm_90 \
    -Wno-deprecated-gpu-targets \
    "$CASE_DIR/copybench.cu" -o "$APP" -lcudart || fail "workload did not build"
  pcomm_load_runtime
fi

# ---- the workload BARE first, then under pcomm ----------------------------
#
# Both, in that order, and it is not ceremony. A capture of a workload that did
# not work is indistinguishable from a tracer that captured nothing, and this
# project has spent several allocations on that confusion. The bare run also
# prints the oracle — copybench's own count of every copy it performed — so the
# log carries the expected numbers beside the captured ones.
echo "=== bare run (no tracer) ==="
timeout 300 "$APP" "$ITERS" "$MSG_BYTES" "$GPUS"
bare_rc=$?
[ $bare_rc -eq 0 ] || fail "workload does not run on this host without pcomm (rc=$bare_rc)"

# ONE process, and it has to SAY it is rank 0.
#
# There is no MPI here, so none of the launcher variables Config::from_env looks
# for is set, and the engine falls back to the pid — which is correct behaviour
# (concurrent unranked processes must not clobber each other's streams) and
# produces a seven-digit rank id. suite/asserts/ranks_complete then reports both
# halves of the same fact: rank 0 "silent" and rank 1652840 "not launched". Its
# own comment names this exact case. Every other case in the suite is launched
# under mpirun or srun and never meets it.
export PCOMM_RANK=0

rm -rf "$OUT" "$OUT.csv" "$OUT.pq"; mkdir -p "$OUT"

echo "=== traced run ==="
# -p cupti alone. Adding gpumem here would be a different experiment and a
# broken one: NVBit takes the process's only CUPTI subscriber, `pcomm run` drops
# cupti and says so, and the capture would legitimately hold no copies at all.
# link_health.memcpy_capture_unavailable is what states that case; it is
# exercised in tests/comm_edges_rule.sh, which can construct it without a GPU.
PCOMM_VERBOSE=1 timeout 300 "$PCOMM" run -p cupti -o "$OUT" -- \
  "$APP" "$ITERS" "$MSG_BYTES" "$GPUS"
rc=$?; echo "traced exit=$rc"
if [ $rc -ne 0 ]; then
  echo "FAILED: traced run did not complete (rc=$rc; 124 = timed out)."
  mkdir -p "$OUT.csv"
  printf '{"case":"23_cuda_memcpy","ok":false,"checks":0,"rc":%d}\n' "$rc" > "$OUT.csv/result.json"
  exit 1
fi

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
