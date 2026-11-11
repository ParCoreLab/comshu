#!/bin/bash
# 04_llmc round 2 — llm.c's GPT-2 trainer on du04 (4x A100, one node, no queue),
# under `comshu run -p nccl,kernel`, then through `comshu export --parquet` and
# `comshu link` so the DERIVED tables can be judged.
#
#   bash tests/hpc/apps/04_llmc/run-r2.sh
#
# Round 1 (run.sh, beside this file) stopped at the raw NCCL tables over a CSV
# export. This script differs in three ways and only three:
#   * it adds `-p kernel`, so the capture holds the NCCL device kernels as well
#     as the calls that enqueued them. No case in tests/hpc/cases/ asserts
#     anything about gpu_kernel; this is the first workload pointed at it.
#   * it exports Parquet and runs `comshu link`, which builds comm_edges,
#     rank_identity, transport_mix, object_access and link_health. link_health
#     is expected EMPTY — a row in it is a defect in the link.
#   * it adds an NCCL protocol arm (knob B): same binary, same command line,
#     NCCL_ALGO/NCCL_PROTO pinned. The application's calls cannot see it; the
#     device kernel names must.
#
# FIVE RUNS, one bare:
#   bare   -z 0                        the workload works. Not optional.
#   z0     -z 0   -p nccl,kernel       data-parallel: 76 all-reduces per step
#   z1     -z 1   -p nccl,kernel       ZeRO-1: reduce-scatter + all-gather
#   z0sim  -z 0   -p nccl,kernel       + NCCL_ALGO=Ring NCCL_PROTO=Simple
#   z0nk   -z 0   -p nccl              control: does adding kernel perturb nccl?
#
# du04 traps (all inherited from round 1's NOTES.md):
#   * /usr/local/cuda is compiler-only; nvcc lives in the HPC SDK tree.
#   * llm.c needs the CUDA-13 cudaMemAdvise patch in this directory to compile.
#   * cublas is in math_libs, not beside nvcc; OpenMPI is in /usr/local.
# Nothing here uses NVBit, so the missing nvdisasm is not in play.
set -eux

HPCSDK=${HPCSDK:-/opt/nvidia/hpc_sdk/Linux_x86_64/25.9}
export PATH="$HPCSDK/cuda/13.0/bin:$PATH"
export LD_LIBRARY_PATH="$HPCSDK/math_libs/13.0/lib64:$HPCSDK/cuda/13.0/lib64:/usr/local/lib:${LD_LIBRARY_PATH:-}"

BUILD=${BUILD:-/DATA/missa18/build-llmc-r2}       # DuckDB + nccl + cupti
LLMC=${LLMC:-$HOME/llmc}                          # karpathy/llm.c at f1e2ace
OUT_ROOT=${OUT_ROOT:-/DATA/missa18/comshu-apps-r2}
COMSHU="$BUILD/comshu/comshu"
export LD_LIBRARY_PATH="$BUILD/libcomshu:$LD_LIBRARY_PATH"
# Two provider directories, colon-separated: the nccl provider is a provider,
# the kernel provider is a gpu-backend, and they are built to different trees.
export COMSHU_PROVIDER_PATH="$BUILD/providers/nccl:$BUILD/gpu-backends/cupti"

NP=4
COMMON="-e d6 -b 2 -t 1024 -x 4 -v 1000 -m 1 -s 0 -h 0"
TOK=$((2 * 1024 * NP))
mkdir -p "$OUT_ROOT"
cd "$LLMC"

echo "########## BARE — no comshu. If this fails nothing below means anything."
mpirun -np $NP ./train_gpt2cu $COMMON -z 0 -d $TOK 2>&1 | tail -25
echo "bare pipeline status: ${PIPESTATUS[0]}"

run_capture() {                 # $1 = label, rest = llm.c args
  local label="$1"; shift
  local out="$OUT_ROOT/$label"
  local prov="${PROV:-nccl,kernel}"
  rm -rf "$out" "$out.pq"
  echo "########## CAPTURE $label : -p $prov : $*"
  mpirun -np $NP -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH ${MPIENV:-} \
    "$COMSHU" run -p "$prov" -o "$out" -- ./train_gpt2cu $COMMON "$@" 2>&1 | tail -20
  echo "=== comshu read"
  "$COMSHU" read "$out"
  echo "=== export + link"
  "$COMSHU" export "$out" --parquet "$out.pq"
  "$COMSHU" link "$out.pq"
  ls "$out.pq"
}

PROV=nccl,kernel run_capture z0    -z 0 -d $TOK
PROV=nccl,kernel run_capture z1    -z 1 -d $TOK
PROV=nccl,kernel MPIENV="-x NCCL_ALGO=Ring -x NCCL_PROTO=Simple" \
                 NCCL_ALGO=Ring NCCL_PROTO=Simple run_capture z0sim -z 0 -d $TOK
PROV=nccl        run_capture z0nk  -z 0 -d $TOK
