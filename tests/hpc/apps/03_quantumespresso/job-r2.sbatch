#!/bin/bash
# 03_quantumespresso, ROUND 2 — does `transport_mix` see what QE's own
# decomposition did to the wire?
#
# READ PREDICTION.md FIRST. It was committed (4b83a2f) before this script had
# ever been run, and everything below is its instrument.
#
# THE EXPERIMENT, in one paragraph. QE partitions MPI_COMM_WORLD by integer
# division, never by discovery: nproc_pool = nproc/npool and my_pool_id =
# mype/nproc_pool (Modules/mp_pools.f90:73,79), so pools are CONTIGUOUS BLOCKS
# of ranks, and the FFT transpose — the dominant primitive in plane-wave DFT —
# runs on sub-communicators of intra_bgrp_comm, i.e. strictly inside the pool
# (FFTXlib/src/fft_scatter.f90). Launch 8 ranks block-placed 4 per node and a
# pool spans a node boundary exactly when npool < nodes. So `-npool 2` makes
# every transpose peer same-node, the transpose leaves the fabric entirely, and
# transport_mix must show that — with NOTHING else changed.
#
# WHY THIS IS NOT ROUND 1 AGAIN. Round 1 ran 4 nodes x 1 rank, which made every
# pool boundary a node boundary, so -npool could only move the RANK matrix. Same
# knob, different topology, different observable: the untested table rather than
# the tested one.
#
#   sbatch tests/hpc/apps/03_quantumespresso/job-r2.sbatch
#
#SBATCH --job-name=pcomm-app-qe-r2
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
# TWO NODES, FOUR RANKS EACH, FOUR GPUS EACH — and every part of that shape is
# load-bearing:
#   4 ranks/node   so that npool=2 puts a whole pool inside one node. This is
#                  the inverse of round 1's choice and it is the experiment.
#   2 nodes        so that npool=1 forces the same traffic across the fabric.
#   --gres=gpu:4   so the four GPUs are DISTINGUISHABLE. Under round 1's gpu:1
#                  every rank saw device 0 and a `device` column that always
#                  said 0 was indistinguishable from one that said nothing —
#                  which makes rank_identity_names_a_device-r2.sql unable to
#                  fail, and it is predicted to fail.
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=01:00:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
CASE_DIR="$PCOMM_ROOT/tests/hpc/apps/03_quantumespresso"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"

# gcc-13.2 for the engine's libstdc++ (GLIBCXX_3.4.31 — without it `pcomm read`
# and `pcomm export` die and the export directory is never created, which reads
# exactly like "the workload wrote nothing"), then QE's own stack.
#
# NOT pcomm_load_runtime: it loads nvidia-hpc-sdk/25.7, whose HPC-X is 2.22.1,
# while pw.x is LINKED against 23.11's HPC-X 2.16 (round 1: ldd shows
# .../hpcx-2.16/ompi/lib). Both pcomm providers here are symbol interposition
# and ucx_deep additionally patches a table read out of the LOADED libucx, so
# they must sit in front of the libucp that QE's own mpirun actually dlopens.
#
# ONE MODULE PER LINE, and that is not style: `module load nvidia-hpc-sdk mkl
# quantumespresso/...` on one line silently loads NOTHING (the QE module declares
# prereq() and gives no error), leaving pw.x off PATH.
module load gcc/13.2.0                        >/dev/null 2>&1
module load nvidia-hpc-sdk/23.11              >/dev/null 2>&1
module load mkl                               >/dev/null 2>&1
module load quantumespresso/7.5-nvidia-nvhpcx >/dev/null 2>&1
command -v pw.x >/dev/null || { echo "!! pw.x not on PATH — module load failed"; exit 1; }
echo "pw.x = $(command -v pw.x)"

# The nvidia-hpc-sdk module's own warning: "NVPCX uses its own implementation of
# MPI that does not offer srun support. Programs compiled with NVPCX must use
# 'mpirun --bind-to none' instead of 'srun'."
export SLURM_CPU_BIND=none

WORK="$HOME/qe-app"
IN="$WORK/ausurf.in"
[ -f "$IN" ] || { echo "!! no $IN — rsync the case inputs first"; exit 1; }

# --------------------------------------------------------- pre-flight -------
# All of this is free and each line answers a question that would otherwise cost
# the allocation. It is printed BEFORE anything runs so the job's own output is
# self-contained.

echo "=== which UCX will actually be loaded ==="
# ucx_deep patches uct_iface_ops_t and guards on the ops-table size. QE loads
# HPC-X 2.16 / UCX 1.15, which is OLDER than anything this provider has been run
# against (case 21 measured runtime 1.18.0 against a 1.19.0 build). Whether the
# guard accepts it is THE open question of this round and cannot be answered
# offline — `pcomm list` documents what a provider captures, not what it
# requires. uct_binding answers it after the fact; this says what it was asked.
ldd "$(command -v pw.x)" 2>/dev/null | grep -Ei "libucp|libucs|libuct|libmpi" | sed 's/^/    /'
for d in $(echo "$LD_LIBRARY_PATH" | tr ':' ' '); do
  [ -e "$d/../bin/ucx_info" ] && { "$d/../bin/ucx_info" -v 2>/dev/null | head -3 | sed 's/^/    /'; break; }
done

echo "=== managed memory: the link line, which is where this question is settled ==="
# plans/todos.txt item 2 asks whether mem_object.device is a lie for migrating
# allocations, and apps/README.md nominates QE as the application that would
# make it live. It does not, and this is the CHEAP half of the evidence — the
# other half is in the source and is cited in meta-r2.json.
#
# DO NOT try to answer this with an LD_PRELOAD interposer. Round 1 did, on all
# four ranks, and measured ZERO for every counter including plain cudaMalloc on
# a run with 3.73 s of GPU time: NVHPC's OpenACC runtime does not reach the CUDA
# allocators through the dynamic symbol table, and CUDA 12 hands out driver entry
# points via cuGetProcAddress. Reading that zero as "no managed memory" is a
# false positive that looks exactly like the true answer.
#
# -gpu=managed and -gpu=unified each link exactly one library, libnvhpcman.so,
# and nothing else does. libnvhpcatm.so is ATOMICS and is a red herring: it is
# linked by the MPI stack on every build.
ldd "$(command -v pw.x)" 2>/dev/null | grep -Ei "nvhpcman|nvhpcmanaux|nvhpcatm" | sed 's/^/    /' \
  || echo "    (no libnvhpcman.so and no libnvhpcmanaux.so — NOT a managed build)"
nm -D "$(command -v pw.x)" 2>/dev/null | grep -Ei "MallocManaged|MemAllocManaged|MallocHost|uacc_alloc" | sed 's/^/    /'

echo "=== do the three providers compose into one pass? ==="
"$PCOMM" run --dry-run -p ucx,ucx_deep,ib -o /dev/null -- /bin/true 2>&1 | sed 's/^/    /'

# ---------------------------------------------------------------- MPI -------
# Each option is a correctness requirement, not a preference:
#   pml ucx             — the layer the `ucx` provider hooks; ob1 leaves it empty.
#   coll_hcoll_enable 0 — hcoll implements collectives on its OWN transport and
#                         never calls ucp_tag_send_nbx, which would hide the
#                         entire all-to-all this study is about.
#   btl ^openib         — no legacy verbs BTL racing UCX for the HCA.
#   map-by ppr:4:node   — BLOCK placement, so node(r) = r/4 and the pool
#                         arithmetic composes with it. Round-robin placement
#                         would invert the whole experiment, silently.
#
# UCX_TLS IS DELIBERATELY NOT SET, and neither is tag matching. Round 1 measured
# that rc_verbs cannot carry this CUDA-aware application's payload at all
# (ibv_reg_mr ... md supports: host), and forcing UCX_RC_MLX5_TM_ENABLE=y to make
# tag_flow non-empty would be tuning the fabric until the tracer looks good.
# What UCX chose is a MEASUREMENT this capture carries, in uct_iface.
MPIOPTS=(--bind-to none --map-by ppr:4:node
         -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH
         -x ESPRESSO_PSEUDO -x OMP_NUM_THREADS
         -mca pml ucx -mca coll_hcoll_enable 0 -mca btl ^openib)
export OMP_NUM_THREADS=1
export ESPRESSO_PSEUDO="$WORK"

# -ndiag 1 IN BOTH ARMS. It partitions the ScaLAPACK ortho grid, a DIFFERENT
# communicator whose default size is a function of nproc_bgrp — the very
# quantity -npool changes. At nproc_bgrp 8 and 4 the default happens to be the
# same 2x2 grid, but "the same by arithmetic today" is not an attribution.
# -ntg is NOT passed as anything but 1: Modules/mp_global.f90:83-84 makes
# ntg_ > 1 a hard errore under __CUDA, and x_ac_qe_cuda.m4:91 defines -D__CUDA
# unconditionally for any GPU build.
QE_FLAGS_COMMON=(-nband 1 -ntg 1 -ndiag 1)

qe_log() {  # qe_log <dir> — the application's OWN log, which is the oracle
  grep -E "Parallel version|Number of MPI processes|MPI processes distributed|K-points division|R & G space division|FFT dimensions|sticks:|Min |Max |Sum |number of k points|GPU acceleration|Davidson diagonalization|End of self-consistent|convergence NOT|JOB DONE" \
       "$1/pw.out" 2>/dev/null | head -40
}

# ============================ RUNS 0a / 0b: BARE ============================
# NOT OPTIONAL (apps/README.md): "a capture of a workload that did not work is
# indistinguishable from a tracer that captured nothing, and that confusion has
# cost this project several slots." Both arms run bare FIRST, before pcomm is
# involved at all, so the two logs also establish the oracle:
#
#   `R & G space division: proc/nbgrp/npool/nimage` must read 8 then 4;
#   `sticks: dense ... Max` IS MAXVAL(ncp_) in fft_scatter.f90:663, so it is the
#   direct check on meta-r2.json's fft_ncpx (predicted 1480 then 2960) without
#   trusting the ceil().
#
# pw.x ALWAYS EXITS 139 ON MN5. It reaches JOB DONE and then takes SIGSEGV in
# teardown; round 1 attributed that with a control run carrying no pcomm and no
# LD_PRELOAD. Do not read a nonzero exit as "the workload failed" — grep JOB DONE.
for cfg in "bare-npool1 1" "bare-npool2 2"; do
  set -- $cfg; label="$1"; npool="$2"
  d="$WORK/$label"; rm -rf "$d"; mkdir -p "$d"; cd "$d"; ln -sf "$WORK"/*.UPF .
  echo "=== [$label] BARE, no pcomm, npool=$npool — the oracle and the control ==="
  timeout 900 mpirun -np 8 "${MPIOPTS[@]}" \
      pw.x -npool "$npool" "${QE_FLAGS_COMMON[@]}" -in "$IN" > "$d/pw.out" 2> "$d/pw.err"
  echo "[$label] pw.x exit=$?   (139 = 128+SIGSEGV in teardown; JOB DONE below is the science)"
  echo "[$label] JOB DONE count = $(grep -c 'JOB DONE' "$d/pw.out")"
  qe_log "$d"
  cd "$WORK"
done

echo "=== THE ORACLE, read off the two bare logs ==="
echo "  predicted transpose chunk = fft_nr3px * fft_ncpx * 16"
echo "    npool=1: (288/8) * ceil(11837/8) * 16 = 36 * 1480 * 16 =   852480 B"
echo "    npool=2: (288/4) * ceil(11837/4) * 16 = 72 * 2960 * 16 =  3409920 B"
echo "  (3409920 is what round 1 MEASURED at 4 ranks with npool=1 — a different"
echo "   rank count and a different flag — because the formula depends on"
echo "   nproc_bgrp and on nothing else. That is what makes it an oracle.)"
grep -A6 "sticks:" "$WORK"/bare-npool1/pw.out "$WORK"/bare-npool2/pw.out 2>/dev/null | sed 's/^/    /'

# ====================== RUNS 1 and 2: THE TWO CAPTURES ======================
# `npool` is the only difference: same binary, same input, same 8 ranks on the
# same 2 nodes with the same placement, same MPI and UCX settings, same
# providers, same mode.
#
# PROVIDERS: ucx,ucx_deep,ib. All three are hook-based and non-exclusive, so
# they share one pass and one engine (the --dry-run above confirms it before a
# rank starts). ucx_deep is the whole point — round 1 established that on the
# default transport pcomm records NO wire traffic at all for this application,
# and the 800-range is the layer that is supposed to close exactly that gap.
# `ib` is retained as the CONTROL: verbs_doors_stay_blind-r2.sql asserts that
# ib_post_send stays empty while ib_device does not, so a change of transport
# under us is visible instead of inherited.
#
# THE DEFAULT MODE, NOT `-m simple`, and the reason is a black-box one. Round 1
# used simple to drop 1.9 M ucx_worker_progress rows, citing core/vocab.hpp for
# what simple keeps. That file is not readable from outside the project and no
# user-facing documentation says whether `simple` retains uct_send — this
# round's headline table. Running at the default costs disk; guessing wrong
# costs the allocation and produces an empty table with an innocent explanation.
# Recorded in QUESTIONS.md.
for cfg in "qe-r2-npool1 1" "qe-r2-npool2 2"; do
  set -- $cfg; label="$1"; npool="$2"
  OUT="$(pcomm_out "$label")"; rm -rf "$OUT"; mkdir -p "$OUT"
  d="$WORK/$label"; rm -rf "$d"; mkdir -p "$d"; cd "$d"; ln -sf "$WORK"/*.UPF .
  echo "=== [$label] pcomm -p ucx,ucx_deep,ib  npool=$npool  UCX_TLS=<default> -> $OUT ==="
  timeout 900 mpirun -np 8 "${MPIOPTS[@]}" \
      "$PCOMM" run -p ucx,ucx_deep,ib -o "$OUT" -- \
      pw.x -npool "$npool" "${QE_FLAGS_COMMON[@]}" -in "$IN" > "$d/pw.out" 2> "$d/pw.err"
  echo "[$label] exit=$?   JOB DONE count = $(grep -c 'JOB DONE' "$d/pw.out")"
  qe_log "$d"
  cd "$WORK"

  echo "--- [$label] captured streams ---"
  "$PCOMM" read "$OUT"
  "$PCOMM" export "$OUT" --csv "$OUT.csv"
  pcomm_derive "$OUT"

  # READ THIS BEFORE ANYTHING ELSE. pcomm.fbs type 806: "accepted = 0 is why
  # uct_send is empty, and saying so is the whole reason this record exists."
  # If it is 0 on UCX 1.15, every claim about transport_mix in this job is
  # unanswerable rather than false, and the finding is claim type 4 — the
  # transport layer is blind on the stack the science applications load.
  echo "--- [$label] uct_binding: did the layout guard let ucx_deep patch? ---"
  awk -F, 'NR==1{print "    "$0} NR>1&&NR<6{print "    "$0}' "$OUT.csv"/uct_binding.csv 2>/dev/null \
    || echo "    (no uct_binding.csv — the ucx_deep provider produced nothing at all)"
done

# =============================== the diff ==================================
echo "=== transport_mix, side by side — THE HEADLINE ==="
for label in qe-r2-npool1 qe-r2-npool2; do
  OUT="$(pcomm_out "$label")"
  echo "--- $label"
  if [ -d "$OUT.pq" ]; then
    # Grouped by scope AND tl_name. `scope` is visibly the column meant for
    # "did this leave the node" and its value vocabulary is published nowhere a
    # black-box user may read, so it is PRINTED here and asserted on nowhere —
    # the assertion binds to tl_name, which carries UCX's own transport names.
    "$PCOMM" sql "$OUT.pq" \
      "SELECT scope, tl_name, op, sum(ops) AS ops, sum(out_bytes) AS out_bytes,
              sum(in_bytes) AS in_bytes, sum(advertised_bytes) AS advertised
       FROM transport_mix GROUP BY 1,2,3 ORDER BY 4 DESC" 2>&1 | sed 's/^/    /'
    echo "  -- tag_flow: the only join between the 600s and the 800s --"
    "$PCOMM" sql "$OUT.pq" \
      "SELECT status, count(*) AS flows, sum(ucp_sends) AS ucp_sends,
              sum(uct_ops) AS uct_ops, sum(doors) AS doors
       FROM tag_flow GROUP BY 1 ORDER BY 2 DESC" 2>&1 | sed 's/^/    /'
    echo "  -- rank_identity: does a ucx-only capture know which GPU a rank had? --"
    "$PCOMM" sql "$OUT.pq" "SELECT * FROM rank_identity ORDER BY rank" 2>&1 | sed 's/^/    /'
    echo "  -- link_health --"
    "$PCOMM" sql "$OUT.pq" "SELECT finding, severity, n, note FROM link_health WHERE n > 0 ORDER BY severity" 2>&1 | sed 's/^/    /'
  else
    echo "    (no derived layer — raw uct_send op counts follow)"
    # rank,ticks,op,... — the CSV export prepends `rank` to the record's own
    # column list, which itself begins "ticks,op", so op is field 3.
    awk -F, 'NR>1{c[$3]++} END{for (o in c) print "    ", o, c[o]}' \
        "$OUT.csv"/uct_send.csv 2>/dev/null
  fi
done

# ------------------------------------------------------------- checks -------
bash "$CASE_DIR/check-r2.sh" \
     "$(pcomm_out qe-r2-npool1).csv" \
     "$(pcomm_out qe-r2-npool2).csv" \
     "$CASE_DIR"
