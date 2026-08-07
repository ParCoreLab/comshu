#!/bin/bash
# PROBE — how late do we install into an ibv_context?
#
# Not a suite case. It answers one question and then either becomes a fix or
# gets deleted. See plans/PLAN-20260807-0130-context-coverage-and-next.
#
# Object-ops hooks (post_send, poll_cq, reg_dm_mr, alloc_null_mr) live INSIDE an
# ibv_context: install() patches function pointers in one context, so a context
# we have not installed into is invisible to every one of them at once. That was
# the whole of the close-accounting bug (job 44369505: every unmatched close on a
# context we had never seen), and it was fixed by installing on FIRST SIGHT
# through any hooked door rather than only at ibv_open_device.
#
# First sight is a floor, not a guarantee. A context is instrumented at its first
# call through a door we hook, so whatever it did before that is missed. API
# ordering makes the window narrow — a send needs a QP and a QP needs a PD, and
# both are hooked doors — but poll_cq on a CQ created before any PD is genuinely
# exposed, and this project has now twice reasoned about ordering and been wrong.
#
# So measure it instead of arguing it. PCOMM_TRACE_CTX_INSTALL=1 prints, at
# unload, one line per door that ever installed a context:
#
#   every install at open_device      -> the window is zero by construction, and
#                                        mlx5dv_open_device would buy nothing here
#   any install at a later door       -> that context ran uninstrumented between
#                                        its open and that door; the window is
#                                        real and the named door bounds it
#
# Case 02's configuration exactly (providers: ib), because that is the case whose
# capture this mechanism changed. Note `-p ib` means the mlx5dv provider is NOT
# loaded, which is why first-sight install had to be the general mechanism:
# interposing mlx5dv_open_device could never have helped this run.
#
# POSITIVE CONTROL: a [pcomm-ctxinstall] total= line from every rank. If none
# appear the diagnostic did not run and nothing below is a finding — an earlier
# probe here mis-stated its own control and reported on a run that never traced.
#
#SBATCH --job-name=pcomm-probe-ctxinstall
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:08:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
export LD_LIBRARY_PATH="$NCCL_HOME/lib:$LD_LIBRARY_PATH"

APP="$HOME/nccl_allreduce"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/nccl_allreduce.cu" -o "$APP" \
    -I"$NCCL_HOME/include" -L"$NCCL_HOME/lib" -lnccl -lcudart
  pcomm_load_runtime
fi

OUT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/probe-ctxinstall-${SLURM_JOB_ID:-local}"
rm -rf "$OUT"; mkdir -p "$OUT"

# Same configuration as case 02 — same providers, same NCCL pinning — because a
# probe that changes the workload cannot be compared against the board it is
# explaining. Only the iteration count drops: contexts are opened at init, so 2
# iterations observe exactly the same set of installs that 20 would.
mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x PCOMM_TRACE_CTX_INSTALL=1 \
  -x NCCL_DEBUG=WARN -x NCCL_P2P_DISABLE=1 \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$PCOMM" -p ib -o "$OUT" -- "$APP" 8388608 2 2>&1 | tee "$OUT/run.log"
echo "mpirun exit=${PIPESTATUS[0]}"

echo "=== POSITIVE CONTROL: did the diagnostic run, and on how many ranks? ==="
printf 'ranks reporting: %s (expect 8)\n' \
  "$(grep -c 'pcomm-ctxinstall.*total=' "$OUT/run.log")"

echo "=== THE ANSWER: which door installed each context ==="
awk '/pcomm-ctxinstall/ && /door=/ {
       for (i=1;i<=NF;i++) { if ($i ~ /^door=/) d=substr($i,6);
                             if ($i ~ /^contexts=/) c=substr($i,10)+0 }
       n[d]+=c }
     END { for (k in n) printf "  %-14s %d contexts (all ranks)\n", k, n[k] }' \
  "$OUT/run.log" | sort -k2 -rn

echo "=== the verdict: any context installed later than open_device? ==="
awk '/pcomm-ctxinstall/ && /total=/ {
       for (i=1;i<=NF;i++) { if ($i ~ /^total=/) t+=substr($i,7)+0
                             if ($i ~ /^late=/)  l+=substr($i,6)+0 } }
     END { printf "  contexts=%d late=%d\n", t, l
           if (l==0) print "  WINDOW IS ZERO — every context installed at ibv_open_device."
           else      print "  WINDOW IS REAL — see the door table above for what bounds it." }' \
  "$OUT/run.log"

# If the window is real, the next question is whether anything was actually
# missed in it, and unmatched closes are the one symptom that is visible from
# outside a context. Zero here with late>0 means the window exists but nothing
# used it — worth recording as measured rather than closing on principle.
echo "=== unmatched closes, for continuity with 44369505 (expect 0) ==="
printf 'unmatched lines: %s\n' "$(grep -c 'pcomm-unmatched' "$OUT/run.log")"

echo "=== per-rank detail, unaggregated ==="
grep -h 'pcomm-ctxinstall' "$OUT/run.log" | sort | uniq -c | sort -rn
