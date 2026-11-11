#!/bin/bash
# 04_llmc on MareNostrum 5 — llm.c's GPT-2 trainer across two nodes, captured
# with BOTH layers at once: `-p nccl,ib` records the collective the application
# asked for AND the wire NCCL turned it into, in one execution.
#
#   while squeue -u koc296797 -h | grep -q .; do sleep 60; done
#   sbatch tests/hpc/apps/04_llmc/job.sbatch
#
#SBATCH --job-name=comshu-app-04-llmc
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:25:00
#SBATCH --output=%x.%j.out
#
# FOUR CONFIGURATIONS IN ONE ALLOCATION, because the queue slot is the scarce
# resource and four agents serialise through one of them (apps/README.md):
#
#   bare    -z 0            no comshu. If this fails nothing below means anything.
#   z0      -z 0            plain data-parallel — one all-reduce per gradient tensor
#   z1      -z 1            ZeRO-1 — reduce-scatter + all-gather instead
#   z0d2    -z 0, -d x2     grad_accum_steps=2, every other variable identical
#
# z0 vs z1 is the config-knob comparison. z0 vs z0d2 is the invariance:
# multi_gpu_async_reduce_gradient is called under `if(last_step)`
# (train_gpt2.cu:929, :953), so doubling the accumulated batch must not change a
# single collective count.

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$COMSHU_SDK/math_libs/12.9/lib64:$LD_LIBRARY_PATH"

LLMC="${LLMC:-$HOME/llmc}"                        # karpathy/llm.c at f1e2ace
APP="$LLMC/train_gpt2cu"

# Built on the LOGIN NODE, not here — a compile inside the allocation spends a
# queue slot on something that needs no GPU. Kept as a fallback so the script is
# self-contained. `-ccbin=mpic++` is how case 02 and case 20 reach MPI; the
# math_libs tree is where the SDK keeps cublas, which is not beside nvcc. The
# CUDA-13 cudaMemAdvise patch in this directory is a no-op here (nvcc is 12.9,
# so CUDART_VERSION < 13000 takes the original arm) but is applied anyway so the
# two machines build the same source.
if [ ! -x "$APP" ]; then
  comshu_load_build
  ( cd "$LLMC" && patch -p1 -N < "$COMSHU_ROOT/tests/hpc/apps/04_llmc/llmc-cuda13-memadvise.patch" || true
    OMPI_CXX=g++ nvcc --threads=0 -t=0 --use_fast_math -std=c++17 -O3 \
      --generate-code arch=compute_90,code=[compute_90,sm_90] \
      -DMULTI_GPU -DUSE_MPI -DENABLE_BF16 -ccbin=mpic++ \
      train_gpt2.cu -o train_gpt2cu \
      -I"$COMSHU_SDK/math_libs/12.9/include" -L"$COMSHU_SDK/math_libs/12.9/lib64" \
      -lcublas -lcublasLt \
      -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl \
      -L"$COMSHU_SDK/cuda/lib64/stubs" -lnvidia-ml )
  comshu_load_runtime
  export LD_LIBRARY_PATH="$NCCL_HOME/lib:$COMSHU_SDK/math_libs/12.9/lib64:$LD_LIBRARY_PATH"
fi

# gpt2-tiny (-e d6): L=6, C=384, Vp=50304, max_seq_len=1024 -> 30,357,504
# parameters, which llm.c prints. -v 1000 leaves a validation only at step 0 and
# at the final step; -s 0 and -h 0 switch off sampling and HellaSwag, neither of
# which communicates. -m 1 is one validation batch.
#
# -b 2 rather than llm.c's default 4: the loader asserts
# `shard_ntok >= num_processes*B*T + 1` (dataloader.h:184) on EVERY shard, and
# tinyshakespeare's val split is exactly 32,768 tokens — which 8*4*1024 fails by
# one token and 8*2*1024 clears. Batch size changes no collective in this
# workload, so the two machines run the identical command shape.
NP=8
COMMON="-e d6 -b 2 -t 1024 -x 4 -v 1000 -m 1 -s 0 -h 0"
TOKENS_PER_FWDBWD=$((2 * 1024 * NP))              # 16384

cd "$LLMC"

echo "########## BARE — no comshu, 8 ranks over 2 nodes"
mpirun -np $NP --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  "$APP" $COMMON -z 0 -d $TOKENS_PER_FWDBWD
echo "bare exit=$?"

# NCCL is NOT pinned to Ring/Simple/1-channel here, unlike cases 02 and 05.
# Those pin it because their goldens are over the WIRE, where NCCL's channel and
# protocol choices decide the send count. Nothing asserted in this case is a wire
# quantity: the collective counts come from llm.c's source and no NCCL internal
# choice can move them, and the IB assertion deliberately claims only existence,
# payload and rank agreement. Pinning would make the run less like the
# application people actually run, for no gain in what is checked.
run_capture() {                                   # $1 = label, $2 = providers, rest = llm.c args
  local label="$1" providers="$2"; shift 2
  local out; out="$(comshu_out "llmc-$label")"
  rm -rf "$out"; mkdir -p "$out"
  echo "########## CAPTURE $label  (-p $providers) : $*"
  mpirun -np $NP --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
    -x COMSHU_PROVIDER_PATH -x NCCL_DEBUG=WARN \
    "$COMSHU" run -p "$providers" -o "$out" -- "$APP" $COMMON "$@"
  echo "mpirun exit=$?"
  echo "=== captured streams ($label) ==="; "$COMSHU" read "$out"
  "$COMSHU" export "$out" --csv "$out.csv"
  comshu_derive "$out"
  echo "=== sql_check ($label) ==="
  bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$out.csv" \
       "$COMSHU_ROOT/tests/hpc/apps/04_llmc" || true
}

run_capture z0   nccl,ib -z 0 -d $TOKENS_PER_FWDBWD
run_capture z1   nccl,ib -z 1 -d $TOKENS_PER_FWDBWD
run_capture z0d2 nccl    -z 0 -d $((TOKENS_PER_FWDBWD * 2))

# The breakdown the report quotes. Written here rather than run by hand so the
# numbers in REPORT.md are reproducible from the job's own stdout.
for label in z0 z1 z0d2; do
  out="$(comshu_out "llmc-$label")"
  echo "########## BREAKDOWN $label"
  sqlite3 <<SQL
.mode column
.headers on
.import --csv $out.csv/nccl_collective.csv nc
.import --csv $out.csv/nccl_group.csv ng
SELECT op, datatype, redop, count(*)/$NP AS rows_per_rank,
       sum(CAST(count AS INTEGER))/$NP AS elems_per_rank,
       count(DISTINCT rank) AS ranks
FROM nc GROUP BY op, datatype, redop ORDER BY op, datatype;
SELECT edge, count(*)/$NP AS groups_per_rank FROM ng GROUP BY edge;
SQL
done

echo "########## IB SIDE (z0) — intent vs wire, one execution"
out="$(comshu_out llmc-z0)"
sqlite3 <<SQL
.mode column
.headers on
.import --csv $out.csv/ib_post_send.csv ips
.import --csv $out.csv/nccl_comm.csv ncm
SELECT rank, count(*) AS sends, sum(CAST(length AS INTEGER)) AS bytes,
       min(CAST(length AS INTEGER)) AS min_len, max(CAST(length AS INTEGER)) AS max_len
FROM ips GROUP BY rank ORDER BY CAST(rank AS INTEGER);
SELECT count(*) AS ranks_in_comm, count(DISTINCT comm_hash) AS distinct_hashes FROM ncm;
SQL
