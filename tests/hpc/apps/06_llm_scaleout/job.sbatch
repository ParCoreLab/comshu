#!/bin/bash
# 06_llm_scaleout — llm.c's GPT-2 trainer at 16 GPUs across 4 nodes, captured at
# the collective layer and the wire layer in one execution, to ask whether the
# NCCL view and the transport view agree.
#
# The oracle is PREDICTION.md, committed in d1c4bd2 before this script had ever
# run.
#
# FIVE executions in ONE allocation:
#
#   1. bare, -z 0, NCCL_DEBUG=INFO   the workload works, AND the independent
#                                    topology oracle: NCCL prints one line per
#                                    connection saying `via P2P/direct pointer`
#                                    or `via NET/IB/<hca>`. Everything §3 of
#                                    PREDICTION.md claims is judged against that,
#                                    not against another pcomm table.
#   2. z0   -p nccl,ib               the primary capture
#   3. z1   -p nccl,ib               ZeRO-1: the one number that changes with P
#   4. z0k  -p nccl,ib,kernel        the third layer, if CUPTI loads on MN5
#   5. z0u  -p nccl,ib,ucx,ucx_deep  does transport_mix see NCCL at all? (no)
#
# The bare arm is not optional (apps/README.md).
#
#   sbatch tests/hpc/apps/06_llm_scaleout/job.sbatch
#
# ONE JOB AT A TIME, ACCOUNT-WIDE. Before submitting:
#   while squeue -u koc296797 -h | grep -q .; do sleep 60; done
#
#SBATCH --job-name=pcomm-app-llm-scaleout
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:50:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
CASE="$PCOMM_ROOT/tests/hpc/apps/06_llm_scaleout"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime                          # gcc 13.2 + HPC SDK 25.7 + CUDA
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$PCOMM_SDK/math_libs/12.9/lib64:$LD_LIBRARY_PATH"
# libcupti is the KERNEL backend's own dependency and nothing in common.sh
# puts it on the path. Without it `-p nccl,ib,kernel` fails at exec with
# "train_gpt2cu: error while loading shared libraries: libcupti.so.12", which
# names the application rather than the provider and reads as a broken build
# (job 45330280, z0k arm, exit 127 on all sixteen ranks).
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$PCOMM_SDK/cuda/extras/CUPTI/lib64"

# The private build at the repo checkout, as 02_lammps round 2 uses. The shared
# $PCOMM_ROOT/build is another agent's and apps/README.md says not to rebuild it.
# The cupti backend is NOT in this tree — it builds to gpu-backends/, not
# providers/ — so `-p kernel` needs build-cupti's directory on the path as well.
# 04_llmc round 2 recorded that PCOMM_PROVIDER_PATH takes a colon-separated list
# and that a missing kernel backend makes `pcomm run` exit 1 rather than
# silently capture nothing.
export PCOMM_BUILD="${PCOMM_BUILD_R2:-$PCOMM_ROOT/build-apps06}"
export PCOMM="$PCOMM_BUILD/pcomm/pcomm"
export PCOMM_PROVIDER_PATH="$PCOMM_BUILD/providers/nccl:$PCOMM_BUILD/providers/ibverbs:$PCOMM_BUILD/providers/ucx:$PCOMM_BUILD/providers/ucx_deep:$PCOMM_ROOT/build-cupti/gpu-backends/cupti"
export LD_LIBRARY_PATH="$PCOMM_BUILD/libpcomm:$LD_LIBRARY_PATH"

LLMC="${LLMC:-$HOME/llmc}"
APP="$LLMC/train_gpt2cu"
[ -x "$APP" ] || { echo "!! no $APP — build it on the login node, see 04_llmc/job.sbatch"; exit 1; }

echo "=== stack ==="
"$PCOMM" types | wc -l
echo "--- is GpuDevice (type 528) in this build's type list? PREDICTION.md §4 says no"
"$PCOMM" types | grep -E '^(524|526|528|530)\b' || true
for d in $(echo "$PCOMM_PROVIDER_PATH" | tr ':' ' '); do ls -la "$d"/*.so 2>/dev/null; done
nvidia-smi -L || true

# gpt2-tiny. -b 1 IS FORCED, not chosen: llmc/dataloader.h:186 asserts
# `shard_ntok >= num_processes*B*T + 1` on EVERY shard and tinyshakespeare's
# validation split is exactly 32,768 tokens, which 16*2*1024 fails by one token
# and 16*1*1024 clears. Batch size changes no collective in this workload — the
# gradient tensors are model-shaped — so every count in meta.json is unaffected.
NP=16
COMMON="-e d6 -b 1 -t 1024 -x 4 -v 1000 -m 1 -s 0 -h 0"
TOKENS_PER_FWDBWD=$((1 * 1024 * NP))        # 16384 -> grad_accum_steps = 1

WORK="${PCOMM_APPS_ROOT:-/gpfs/scratch/etur02/koc296797-apps}/llm-scaleout-$SLURM_JOB_ID"
mkdir -p "$WORK" || { echo "!! cannot create $WORK"; exit 1; }
cd "$LLMC" || exit 1

MPI_ENV=(-x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH)
LAUNCH=(mpirun -np $NP --map-by ppr:4:node --bind-to none "${MPI_ENV[@]}")

# ------------------------------------------------------- 0. WHERE THE RANKS ARE
# From the operating system, before any capture exists, so that comm_edges.scope
# and rank_identity are judged against the launcher rather than against another
# pcomm table.
echo "=== PLACEMENT ORACLE: rank -> host ==="
"${LAUNCH[@]}" bash -c 'printf "RANKHOST rank=%03d local=%s host=%s\n" "$OMPI_COMM_WORLD_RANK" "$OMPI_COMM_WORLD_LOCAL_RANK" "$(hostname)"' \
  2>&1 | grep RANKHOST | sort

# --------------------------------------------------------------- 1. BARE run
# NCCL_DEBUG=INFO is the whole reason this arm carries an environment the
# captured arms do not: it is the ONLY independent oracle available below the
# NCCL API, and PREDICTION.md P4.1/P4.3 are stated against it.
echo "########## BARE -z 0 (with NCCL_DEBUG=INFO — the topology oracle)"
"${LAUNCH[@]}" -x NCCL_DEBUG=INFO -x NCCL_DEBUG_SUBSYS=INIT,GRAPH,TUNING \
  "$APP" $COMMON -z 0 -d $TOKENS_PER_FWDBWD 2>&1 | tee "$WORK/bare.log" | tail -30
echo "bare_exit=${PIPESTATUS[0]}"

echo "=== THE TOPOLOGY ORACLE, from NCCL and not from pcomm ==="
echo "--- connections NCCL says crossed the fabric"
grep -oE 'via NET/[A-Za-z0-9_/]*' "$WORK/bare.log" | sort | uniq -c | head
grep -E 'Channel [0-9]+.*(NET|P2P)' "$WORK/bare.log" | sed 's/^.*NCCL INFO //' | sort -u | head -60
echo "--- how many connections are P2P and how many are NET"
printf "  P2P lines: %s\n" "$(grep -c 'via P2P' "$WORK/bare.log")"
printf "  NET lines: %s\n" "$(grep -c 'via NET' "$WORK/bare.log")"
echo "--- the algorithm and protocol NCCL chose per collective size"
grep -oE '[0-9]+ Bytes -> Algo [0-9]+ proto [0-9]+|-> Algo [A-Za-z0-9]+ proto [A-Za-z0-9]+' "$WORK/bare.log" | sort | uniq -c | head -20
echo "--- llm.c's own numbers"
grep -E 'num_parameters|num_processes|total batch size|grad_accum_steps|val loss|train loss' "$WORK/bare.log" | head -20

# ------------------------------------------------------------ 2. the captures
run_capture() {                             # $1 = label, $2 = providers, rest = args
  local label="$1" providers="$2"; shift 2
  local out="$WORK/$label"
  rm -rf "$out" "$out.csv" "$out.pq"; mkdir -p "$out"
  echo "########## CAPTURE $label  (-p $providers) : $*"
  "${LAUNCH[@]}" -x NCCL_DEBUG=WARN "$PCOMM" run -p "$providers" -o "$out" -- \
      "$APP" $COMMON "$@" 2>&1 | tail -25
  echo "capture_${label}_exit=$?"
  ls -d "$out"/rank-* >/dev/null 2>&1 || { echo "!! NO rank-* IN $out — the workload never started"; return 1; }
  echo "--- captured streams ($label) ---"; "$PCOMM" read "$out"
  "$PCOMM" export "$out" --csv "$out.csv"
  pcomm_derive "$out"
  return 0
}

run_capture z0  nccl,ib               -z 0 -d $TOKENS_PER_FWDBWD
run_capture z1  nccl,ib               -z 1 -d $TOKENS_PER_FWDBWD
# The kernel arm is allowed to fail without taking the study with it: MN5 has
# never run CUPTI under pcomm and the backend is built in a different tree.
run_capture z0k nccl,ib,kernel        -z 0 -d $TOKENS_PER_FWDBWD || echo "!! z0k arm failed — the kernel layer is unanswered, the rest stands"
# The transport_mix control. llm.c uses MPI only to bootstrap, so if
# transport_mix is non-empty here it holds the BOOTSTRAP and not the gradient.
run_capture z0u nccl,ib,ucx,ucx_deep  -z 0 -d $TOKENS_PER_FWDBWD || echo "!! z0u arm failed — P4.4 is unanswered, the rest stands"

# ------------------------------------------------------------- 3. assertions
for arm in z0 z1 z0k; do
  [ -d "$WORK/$arm.pq" ] || { echo "########## ASSERTIONS: $arm — no export, skipped"; continue; }
  case "$arm" in z1) v=z1 ;; *) v=z0 ;; esac
  echo "########## ASSERTIONS: $arm  [variant $v] ##########"
  bash "$CASE/run-asserts.sh" "$WORK/$arm.pq" "$CASE" "$v"
  echo "asserts_${arm}_exit=$?"
done

# ------------------------------------------------- 4. the report's raw numbers
PQ="$WORK/z0.pq"
echo "########## NUMBERS: z0 ##########"
echo "=== comm_edges, every row (predicted: every one scope='network') ==="
"$PCOMM" sql "$PQ" "SELECT src_rank, dst_rank, transport, unit, scope, sends, bytes FROM comm_edges ORDER BY bytes DESC"
echo "=== comm_edges rolled up by scope ==="
"$PCOMM" sql "$PQ" "SELECT scope, count(*) AS edges, sum(sends) AS sends, sum(bytes) AS bytes FROM comm_edges GROUP BY scope"
echo "=== the same edges as a node-pair matrix (4 nodes, block placement) ==="
"$PCOMM" sql "$PQ" "SELECT src_rank//4 AS src_node, dst_rank//4 AS dst_node, count(*) AS edges, sum(bytes) AS bytes FROM comm_edges GROUP BY 1,2 ORDER BY 1,2"
echo "=== ib_post_send per rank; the bound is 4 x 242,860,032 = 971,440,128 B total ==="
"$PCOMM" sql "$PQ" "SELECT rank, count(*) AS sends, sum(length) AS bytes, min(length) AS min_len, max(length) AS max_len FROM ib_post_send GROUP BY 1 ORDER BY 1"
"$PCOMM" sql "$PQ" "SELECT count(*) AS posts, sum(length) AS total_bytes FROM ib_post_send"
echo "=== ib_post_send by opcode: how much of it is notification ==="
"$PCOMM" sql "$PQ" "SELECT opcode, count(*) AS n, sum(length) AS bytes, sum(CASE WHEN length=0 THEN 1 ELSE 0 END) AS zero_len FROM ib_post_send GROUP BY 1 ORDER BY n DESC" || true
echo "=== endpoint: does the verbs lane resolve a peer rank for NCCL's QPs ==="
"$PCOMM" sql "$PQ" "SELECT count(*) AS eps, count(peer_rank) AS with_peer_rank, count(DISTINCT rank) AS ranks FROM endpoint"
"$PCOMM" sql "$PQ" "SELECT rank, qp_num, lid, peer_lid, peer_qp_num, peer_rank, transport, sends, bytes FROM endpoint ORDER BY bytes DESC LIMIT 25"
echo "=== rank_identity (predicted 16 rows, 4 node_hash, device = rank%4) ==="
"$PCOMM" sql "$PQ" "SELECT rank, node_ix, node_hash, device, devices, label FROM rank_identity ORDER BY rank"
echo "=== nccl_comm ==="
"$PCOMM" sql "$PQ" "SELECT rank, nccl_rank, nranks, cuda_dev, comm_hash FROM nccl_comm ORDER BY rank"
echo "=== the collective algebra, both stages ==="
for a in z0 z1; do
  [ -d "$WORK/$a.pq" ] || continue
  echo "--- $a"
  "$PCOMM" sql "$WORK/$a.pq" "SELECT op, datatype, count AS elems_each, count(*)/16 AS rows_per_rank, sum(count)/16 AS elems_per_rank FROM nccl_collective GROUP BY 1,2,3 ORDER BY rows_per_rank DESC" || true
  "$PCOMM" sql "$WORK/$a.pq" "SELECT count(*)/16 AS group_rows_per_rank FROM nccl_group"
done
echo "=== transport_mix on the ucx arm: bootstrap only, or gradient too? (P4.4) ==="
if [ -d "$WORK/z0u.pq" ]; then
  "$PCOMM" sql "$WORK/z0u.pq" "SELECT scope, tl_name, sum(ops) AS ops, sum(out_bytes) AS out_b, sum(in_bytes) AS in_b FROM transport_mix GROUP BY 1,2 ORDER BY out_b DESC" || true
  "$PCOMM" sql "$WORK/z0u.pq" "SELECT (SELECT COALESCE(sum(out_bytes)+sum(in_bytes),0) FROM transport_mix) AS uct_bytes, (SELECT COALESCE(sum(length),0) FROM ib_post_send) AS ib_bytes"
  "$PCOMM" sql "$WORK/z0u.pq" "SELECT rank, compiled_version, runtime_version, ops_size, accepted, forced FROM uct_binding LIMIT 4" || true
fi
echo "=== the kernel layer, if it loaded (predicted 35 ncclDevKernel per rank) ==="
if [ -d "$WORK/z0k.pq" ]; then
  "$PCOMM" sql "$WORK/z0k.pq" "SELECT rank, count(*) AS kernels FROM gpu_kernel GROUP BY 1 ORDER BY 1" || true
  "$PCOMM" sql "$WORK/z0k.pq" "SELECT k.rank, count(*) AS nccl_kernels FROM gpu_kernel k JOIN intern_text t ON t.handle = k.name WHERE t.text LIKE '%ncclDevKernel%' GROUP BY 1 ORDER BY 1" || true
fi
echo "=== link_health rubric ==="
"$PCOMM" sql "$PQ" "SELECT finding, severity, n, note FROM link_health ORDER BY severity, n DESC"
echo "=== is there a gpu_device table? PREDICTION.md §4 predicts no ==="
ls "$PQ" | grep -i gpu || echo "  (no gpu_* parquet in the export)"

echo "=== artefacts ==="
ls -la "$WORK"
du -sb "$WORK"/* 2>/dev/null | tail -20
