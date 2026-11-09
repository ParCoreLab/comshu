#!/bin/bash
# 03_quantumespresso — plane-wave DFT (pw.x) under pcomm, on MareNostrum 5.
#
# WHY THIS APPLICATION. Every workload in tests/hpc/cases is a halo exchange or
# a collective on a contiguous buffer. Plane-wave DFT is neither: the FFT grid is
# distributed over planes, so the dominant primitive is MPI_ALLTOALL
# (FFTXlib/src/fft_scatter.f90:715, :792, :433, :531) — and QE nests that
# all-to-all inside a communicator hierarchy whose partition of MPI_COMM_WORLD is
# pure arithmetic in Modules/mp_pools.f90:73-92. Change one flag and the block
# structure of the comm matrix must change in a way derivable from the source
# WITHOUT naming any rank. That is what makes this an assertion and not a golden.
#
# THE PREDICTION, WRITTEN BEFORE THE FIRST CAPTURE (see REPORT.md "Prediction"):
#   pool(r) = r / (ranks/npool), integer division   [mp_pools.f90:73,79]
#   pools are CONTIGUOUS blocks of ranks             [mp_pools.f90:86]
#   ranks=4, npool=1 -> one pool {0,1,2,3}: the FFT all-to-all spans every rank,
#                       so the rank-space matrix is COMPLETE (12 ordered edges).
#   ranks=4, npool=2 -> pools {0,1} and {2,3}: the all-to-all cannot leave a
#                       pool, so cross-pool traffic collapses to the per-SCF
#                       inter-pool reduction (mp_pools.f90:92, sum_band.f90).
#
#   sbatch tests/hpc/apps/03_quantumespresso/job.sbatch
#
#SBATCH --job-name=pcomm-app-qe
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
# FOUR NODES, ONE RANK EACH, ONE GPU EACH — and the shape is load-bearing.
# QE partitions MPI_COMM_WORLD into pools by RANK NUMBER (mp_pools.f90:79), so
# with 4 ranks x 1 node the pool boundary falls on a NODE boundary and every
# intra-pool message is on the wire. With 4 ranks per node an entire pool would
# live inside one node, UCX would pick shared memory for it, and the `ib` half of
# the capture would be empty for exactly the traffic the claim is about.
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=01:50:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
CASE_DIR="$PCOMM_ROOT/tests/hpc/apps/03_quantumespresso"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"

# gcc-13.2 for the engine's libstdc++ (GLIBCXX_3.4.31 — without it `pcomm read`
# and `pcomm export` die with a GLIBCXX version error and the capture looks
# empty), then QE's own stack.
#
# NOT pcomm_load_runtime: that loads nvidia-hpc-sdk/25.7, whose HPC-X is 2.22.1,
# while pw.x was LINKED against 23.11's HPC-X 2.16 (ldd shows
# .../hpcx-2.16/ompi/lib). The UCX provider is symbol interposition, so it must
# sit in front of the libucp that is actually loaded — the one QE's own mpirun
# dlopens. Measured: the provider .so carries a RUNPATH to 2.22.1's ucx/lib but
# 23.11's module puts 2.16's ucx/lib on LD_LIBRARY_PATH, which wins under new
# dtags, and interposition against UCX 1.15 was verified on the login node.
module load gcc/13.2.0                        >/dev/null 2>&1
module load nvidia-hpc-sdk/23.11              >/dev/null 2>&1
module load mkl                               >/dev/null 2>&1
module load quantumespresso/7.5-nvidia-nvhpcx >/dev/null 2>&1
command -v pw.x >/dev/null || { echo "!! pw.x not on PATH — module load failed"; exit 1; }
echo "pw.x = $(command -v pw.x)"

# The nvidia-hpc-sdk module's own warning: "NVPCX uses its own implementation of
# MPI that does not offer srun support. Programs compiled with NVPCX must use
# 'mpirun --bind-to none' instead of 'srun'. Remember to also add
# 'export SLURM_CPU_BIND=none'."
export SLURM_CPU_BIND=none

WORK="$HOME/qe-app"
IN="$WORK/ausurf.in"
[ -f "$IN" ] || { echo "!! no $IN — rsync the case inputs first"; exit 1; }

# MPI settings, pinned for the reason case 02 pins NCCL's: left free, the stack
# chooses by measured bandwidth and anything derived from the capture drifts run
# to run. Each is also a correctness requirement here:
#   pml ucx             — the layer the `ucx` provider hooks; ob1 would leave it empty.
#   coll_hcoll_enable 0 — hcoll implements collectives on its OWN transport and
#                         never calls ucp_tag_send_nbx, which would hide the
#                         entire all-to-all this case is about.
#   btl ^openib         — no legacy verbs BTL racing UCX for the HCA.
MPIOPTS=(--bind-to none --map-by ppr:1:node
         -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH
         -x ESPRESSO_PSEUDO -x OMP_NUM_THREADS
         -mca pml ucx -mca coll_hcoll_enable 0 -mca btl ^openib)
export OMP_NUM_THREADS=1
export ESPRESSO_PSEUDO="$WORK"

# THE TWO CAPTURES THAT CARRY THE CLAIM RUN ON THE DEFAULT TRANSPORT, i.e. on
# QE exactly as a user runs it. An earlier revision pinned UCX_TLS to a verbs
# transport so that ibv_post_send would fire and comm_edges would fill. That was
# a workaround being promoted to the measurement, and it does not survive
# contact with this application:
#
#   job 45082487, UCX_TLS=rc_verbs,ud_verbs,self,sm, both npool runs aborted:
#     ib_md.c:309   ibv_reg_mr(address=0x7fdc44400000, length=3409920,
#                   access=0xf) failed: Bad address
#     ucp_mm.c:62   failed to register address ... on md[3]=mlx5_0:
#                   Input/output error (md supports: host)
#     rndv.c:2360   Assertion `status == UCS_OK' failed
#
# That is not a tuning problem. QE hands MPI GPU DEVICE POINTERS — pw.x is
# CUDA-aware and the FFT transpose buffers live in device memory — and UCX's
# rendezvous protocol must register the user buffer. The rc_verbs memory domain
# is host-only, so the registration fails and the run dies. The finding is about
# reach, not about a flag: for this application, on this stack, the payload never
# traverses a path that a verbs-level tracer can register or observe. That is
# recorded in NOTES.md and REPORT.md as a constraint on what pcomm can see, and
# it is NOT worked around in the captures that carry the -npool claim.
#
# Run 4 still tries rc_verbs, last, with the CUDA memory domain added and
# rendezvous disabled, purely to test whether the wire can be made visible at
# all. It is a bonus: it runs after everything the report depends on, and its
# failure costs nothing.
VERBS_TLS=rc_verbs,ud_verbs,self,sm,cuda_copy,gdr_copy

qe_log() {  # qe_log <dir> — the application's OWN log, which is an oracle
  grep -E "Parallel version|Number of MPI processes|MPI processes distributed|K-points division|R & G space division|FFT dimensions|number of k points|GPU acceleration|Davidson diagonalization|End of self-consistent|convergence NOT|JOB DONE" \
       "$1/pw.out" 2>/dev/null | head -30
}

# ---------------------------------------------------------------- run 0 ------
# BARE, and not optional (apps/README.md): a capture of a workload that did not
# work is indistinguishable from a tracer that captured nothing.
#
# It also carries the managed-memory instrument. plans/todos.txt item 1 asks
# whether mem_object.device is a lie for migrating allocations. That question is
# unanswerable FROM a capture — mem_object records no discriminator for which
# door an allocation came through, so a managed and a device allocation are
# byte-identical in a capture. The door is therefore counted from OUTSIDE, by an
# LD_PRELOAD that is not pcomm and is loaded only here, where it cannot perturb
# anything pcomm records.
PROBE="$WORK/libmanaged_probe.so"
rm -f "$PROBE"
gcc -O1 -fPIC -shared -o "$PROBE" "$WORK/managed_probe.c" -ldl \
  || echo "!! managed probe build failed"

# Run 0a — the SAME bare run with NOTHING preloaded, purely to attribute the
# teardown SIGSEGV job 45081919 recorded. pw.x reached "JOB DONE" and then died
# with signal 11 in both the probed bare run and the unprobed pcomm run, which
# means the probe is not obviously the cause and pcomm is not obviously the
# cause — but "not obviously" is not an attribution. This run has neither, so it
# separates "pw.x segfaults at teardown on MN5" from "something we preloaded
# does". Twelve seconds, and it decides whether a truncated stream is ours.
d="$WORK/bare-clean"; rm -rf "$d"; mkdir -p "$d"; cd "$d"; ln -sf "$WORK"/*.UPF .
echo "=== [bare-clean] npool=1, no pcomm, NO preload — teardown-segfault control ==="
timeout 900 mpirun -np 4 "${MPIOPTS[@]}" \
    pw.x -npool 1 -nband 1 -ntg 1 -in "$IN" > "$d/pw.out" 2> "$d/pw.err"
echo "[bare-clean] pw.x exit=$?   (139 = 128+SIGSEGV; JOB DONE below means the science finished)"
grep -c "JOB DONE" "$d/pw.out"

d="$WORK/bare"; rm -rf "$d"; mkdir -p "$d"; cd "$d"; ln -sf "$WORK"/*.UPF .
echo "=== [bare] npool=1, no pcomm, managed-memory probe preloaded ==="
export MANAGED_PROBE_OUT="$d/managed"
timeout 900 mpirun -np 4 "${MPIOPTS[@]}" -x LD_PRELOAD="$PROBE" -x MANAGED_PROBE_OUT \
    pw.x -npool 1 -nband 1 -ntg 1 -in "$IN" > "$d/pw.out" 2> "$d/pw.err"
echo "[bare] pw.x exit=$?"
qe_log "$d"
echo "=== managed-memory door counts, one file per rank ==="
cat "$d"/managed.* 2>/dev/null || echo "(no probe output at all — not one CUDA allocation door was reached)"
unset MANAGED_PROBE_OUT
cd "$WORK"

# ------------------------------------------------------- runs 1 and 2 --------
# The knob comparison, both configurations inside the ONE allocation. `npool` is
# the only difference: same binary, same input, same 4 ranks on the same 4 nodes,
# same MPI and UCX settings.
#
# run_qe is not factored out over these: `pcomm run` wraps the RANKED binary, not
# mpirun (apps/README.md), so the capture form is a different command line, and
# spelling it out keeps the line the report cites identical to the line that ran.
for cfg in "npool1 1" "npool2 2"; do
  set -- $cfg; label="$1"; npool="$2"
  OUT="$(pcomm_out "qe-$label")"; rm -rf "$OUT"; mkdir -p "$OUT"
  d="$WORK/$label"; rm -rf "$d"; mkdir -p "$d"; cd "$d"; ln -sf "$WORK"/*.UPF .
  # -m simple, not the default Full. Measured on job 45081919's capture:
  # ucx_worker_progress alone was 1,932,658 rows against 125,069 tagged sends —
  # 94 % of the capture, and nothing any assertion here reads. Simple keeps
  # post_send/post_recv/modify_qp and EVERY entity (core/vocab.hpp:79) and drops
  # completions and progress. Smaller also means less exposure to the teardown
  # SIGSEGV below, which truncates whatever has not been flushed.
  #
  # mlx5dv is loaded because on the default transport it is the only provider
  # with anything to say about the wire: job 45081919 recorded ib_post_send = 0
  # rows AND ib_modify_qp with no RTR transition at all (70 rows, none carrying
  # dest_lid), because rc_mlx5 bypasses the verbs doors for CONNECT as well as
  # for SEND. libuct_ib.so's undefined symbols name where it went instead —
  # mlx5dv_devx_qp_modify, mlx5dv_devx_obj_create — and link_sql.hpp's `_owner`
  # view already unions mlx5_devx_qp_connect with ib_modify_qp.
  echo "=== [$label] pcomm -p ib,ucx,mlx5dv -m simple  npool=$npool  UCX_TLS=<default> -> $OUT ==="
  timeout 900 mpirun -np 4 "${MPIOPTS[@]}" \
      "$PCOMM" run -p ib,ucx,mlx5dv -m simple -o "$OUT" -- \
      pw.x -npool "$npool" -nband 1 -ntg 1 -in "$IN" > "$d/pw.out" 2> "$d/pw.err"
  echo "[$label] exit=$?"
  qe_log "$d"
  cd "$WORK"
  echo "--- [$label] captured streams ---"; "$PCOMM" read "$OUT"
  "$PCOMM" export "$OUT" --csv "$OUT.csv"
  pcomm_derive "$OUT"
done

# ---------------------------------------------------------------- run 3 ------
# The blind-spot control. Identical to run 1 except that UCX_TLS is left at its
# default, so UCX picks rc_mlx5. If ib_post_send is populated in run 1 and empty
# here, the missing door is measured in this job — the same application, the same
# traffic, one transport variable — rather than inferred from job 44635003.
OUT="$(pcomm_out qe-npool1-mlx5)"; rm -rf "$OUT"; mkdir -p "$OUT"
d="$WORK/npool1-mlx5"; rm -rf "$d"; mkdir -p "$d"; cd "$d"; ln -sf "$WORK"/*.UPF .
# ---------------------------------------------------------------- run 4 ------
# BONUS, and it runs last for that reason. Can the wire be made observable at all
# for a CUDA-aware application? rc_verbs alone could not register QE's device
# buffers (see the UCX_TLS comment above); this adds the CUDA memory domain and
# turns rendezvous off so large transfers go eager through host bounce buffers,
# which ARE registrable. If ib_post_send fills here and is empty in runs 1-2,
# then the blind spot is the transport's WQE path and not pcomm's hooks, and the
# report can say so with two captures of one application instead of an argument.
# If it aborts like job 45082487 did, that is recorded and nothing else is lost.
OUT="$(pcomm_out qe-npool1-rcverbs)"; rm -rf "$OUT"; mkdir -p "$OUT"
d="$WORK/npool1-rcverbs"; rm -rf "$d"; mkdir -p "$d"; cd "$d"; ln -sf "$WORK"/*.UPF .
echo "=== [npool1-rcverbs] BONUS  UCX_TLS=$VERBS_TLS  UCX_RNDV_THRESH=inf -> $OUT ==="
timeout 900 mpirun -np 4 "${MPIOPTS[@]}" \
    -x UCX_TLS="$VERBS_TLS" -x UCX_RNDV_THRESH=inf \
    "$PCOMM" run -p ib,ucx -m simple -o "$OUT" -- \
    pw.x -npool 1 -nband 1 -ntg 1 -in "$IN" > "$d/pw.out" 2> "$d/pw.err"
echo "[npool1-rcverbs] exit=$?  (a failure here is expected-possible; see NOTES.md)"
grep -cE "JOB DONE" "$d/pw.out" || true
tail -4 "$d/pw.err" | sed 's/^/    /'
cd "$WORK"
if [ -d "$OUT/rank-0" ]; then
  "$PCOMM" read "$OUT"; "$PCOMM" export "$OUT" --csv "$OUT.csv"; pcomm_derive "$OUT"
else
  echo "[npool1-rcverbs] no rank-* directory — the workload never started"
fi

# ---------------------------------------------------------------- checks -----
bash "$CASE_DIR/check.sh" \
     "$(pcomm_out qe-npool1).csv" \
     "$(pcomm_out qe-npool2).csv" \
     "$(pcomm_out qe-npool1-rcverbs).csv" \
     "$CASE_DIR"
