#!/bin/bash
# 04_llmc on du04 — llm.c's GPT-2 trainer under `comshu run -p nccl`, four A100s,
# one node, no queue.
#
#   bash tests/hpc/apps/04_llmc/run.sh
#
# THREE CAPTURES IN ONE PASS, plus a bare run first. The bare run is not optional
# (apps/README.md): a capture of a workload that did not work is
# indistinguishable from a tracer that captured nothing.
#
#   bare    -z 0            the workload runs, and prints num_parameters
#   z0      -z 0            plain data-parallel: one all-reduce per gradient tensor
#   z1      -z 1            ZeRO-1: reduce-scatter + all-gather instead
#   z0d2    -z 0, -d x2     grad_accum_steps=2, everything else identical
#
# The z0/z1 pair is the config-knob comparison (claim 3). The z0/z0d2 pair is the
# invariance: the reduction is under `if(last_step)`, so doubling the accumulated
# batch must NOT change any collective count.
#
# du04 TRAPS, all of them paid for once already — see NOTES.md:
#   * /usr/local/cuda is compiler-only. nvcc is at the HPC SDK path below.
#   * that nvcc is CUDA 13, and llm.c does not build under it without the
#     one-line cudaMemAdvise patch in this directory.
#   * cublas headers/libs live in the SDK's math_libs tree, not beside nvcc.
#   * comshu's nccl provider CMake uses find_path for cuda_runtime_api.h, which
#     does not look in /usr/local/cuda/include on its own.
# Nothing here uses NVBit, so the missing nvdisasm is not in play.
set -eux

HPCSDK=${HPCSDK:-/opt/nvidia/hpc_sdk/Linux_x86_64/25.9}
export PATH="$HPCSDK/cuda/13.0/bin:$PATH"
export CPATH="$HPCSDK/math_libs/13.0/include:$HPCSDK/cuda/13.0/include"
export LIBRARY_PATH="$HPCSDK/math_libs/13.0/lib64:$HPCSDK/cuda/13.0/lib64"
export LD_LIBRARY_PATH="$HPCSDK/math_libs/13.0/lib64:$HPCSDK/cuda/13.0/lib64:/usr/local/lib:${LD_LIBRARY_PATH:-}"

COMSHU_SRC=${COMSHU_SRC:-$HOME/comshu-llmc}          # a checkout of this repo
LLMC=${LLMC:-$HOME/llmc}                          # karpathy/llm.c at f1e2ace
export COMSHU="$COMSHU_SRC/build/comshu/comshu"
export COMSHU_PROVIDER_PATH="$COMSHU_SRC/build/providers/nccl"
OUT_ROOT=${OUT_ROOT:-$HOME/comshu-apps}

# --- comshu, nccl provider only ----------------------------------------------
# CUDA_RT_INCLUDE_DIR must be spelled: providers/nccl/CMakeLists.txt find_path()s
# cuda_runtime_api.h and CMake's default prefix list does not include
# /usr/local/cuda/include.
if [ ! -x "$COMSHU" ]; then
  cmake -B "$COMSHU_SRC/build" -S "$COMSHU_SRC" -DCMAKE_BUILD_TYPE=Release \
        -DCOMSHU_BUILD_TESTS=OFF -DCOMSHU_PROVIDER_NCCL=ON \
        -DCUDA_RT_INCLUDE_DIR=/usr/local/cuda/include -DNCCL_INCLUDE_DIR=/usr/include
  cmake --build "$COMSHU_SRC/build" -j 16
fi

# --- llm.c ------------------------------------------------------------------
# Data: dev/data/tinyshakespeare.py, run on a machine with network. 305,260
# train + 32,768 val tokens is far more than 4 steps needs.
if [ ! -x "$LLMC/train_gpt2cu" ]; then
  ( cd "$LLMC" && patch -p1 -N < "$(dirname "$0")/llmc-cuda13-memadvise.patch" || true
    make train_gpt2cu OPENMPI_DIR=/usr/local GPU_COMPUTE_CAPABILITY=80 )
fi

# gpt2-tiny (d6): L=6, C=384, Vp=50304, max_seq_len=1024 -> 30,357,504 parameters.
# -v 1000 puts a validation only at step 0 and at the final step; -s 0 and -h 0
# switch off sampling and HellaSwag, neither of which communicates. -b 2 is
# chosen so the 32,768-token val split still satisfies the loader's
# `shard_ntok >= num_processes*B*T + 1` (dataloader.h:184) at 8 ranks on MN5, so
# the two machines run the identical command shape.
NP=4
COMMON="-e d6 -b 2 -t 1024 -x 4 -v 1000 -m 1 -s 0 -h 0"
TOKENS_PER_FWDBWD=$((2 * 1024 * NP))

cd "$LLMC"

echo "########## BARE — no comshu. If this fails, nothing below means anything."
mpirun -np $NP ./train_gpt2cu $COMMON -z 0 -d $TOKENS_PER_FWDBWD
echo "bare exit=$?"

# du04 is 4 ranks on 1 node and meta.json's oracle is written for MN5's 8 on 2.
# The workload block IS the oracle, so it has to say which machine ran — but a
# second meta.json in the case directory would be a second thing to keep true.
# Instead the case directory is projected into a temp copy with `ranks` and
# `nodes` rewritten for this host, and the assertions come along unchanged.
CASE_SRC="$(cd "$(dirname "$0")" && pwd)"
CASE_DIR="$(mktemp -d)/04_llmc"
mkdir -p "$CASE_DIR"
cp -r "$CASE_SRC/asserts" "$CASE_DIR/"
python3 -c '
import json, sys
m = json.load(open(sys.argv[1]))
m["workload"]["ranks"] = int(sys.argv[3])
m["workload"]["nodes"] = 1
json.dump(m, open(sys.argv[2], "w"), indent=2)
' "$CASE_SRC/meta.json" "$CASE_DIR/meta.json" "$NP"

run_capture() {           # $1 = label, rest = extra llm.c args
  local label="$1"; shift
  local out="$OUT_ROOT/llmc-$label-${SLURM_JOB_ID:-local}"
  rm -rf "$out"; mkdir -p "$out"
  echo "########## CAPTURE $label : $*"
  mpirun -np $NP -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
    "$COMSHU" run -p nccl -o "$out" -- ./train_gpt2cu $COMMON "$@"
  echo "mpirun exit=$?"
  echo "=== captured streams ==="; "$COMSHU" read "$out"
  "$COMSHU" export "$out" --csv "$out.csv"
  bash "$COMSHU_SRC/tests/hpc/suite/sql_check.sh" "$out.csv" "$CASE_DIR" || true
}

run_capture z0   -z 0 -d $TOKENS_PER_FWDBWD
run_capture z1   -z 1 -d $TOKENS_PER_FWDBWD
run_capture z0d2 -z 0 -d $((TOKENS_PER_FWDBWD * 2))
