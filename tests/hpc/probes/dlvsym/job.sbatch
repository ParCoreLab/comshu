#!/bin/bash
# PROBE — is our `dlvsym` interposer ENTERED at all?
#
# Not a suite case. It answers one question and then either becomes a fix or
# gets deleted. See plans/PLAN-20260806-2320-dlvsym-interposer-not-observed.
#
# The contradiction it exists to break: NCCL resolves its ibv_* entry points
# through dlvsym (nccl src/misc/ibvsymbols.cc, plus ELF on the SDK build case 02
# actually loads), our dlvsym IS exported by libpcomm_engine.so and DOES route
# through on_symbol_load (readelf + the body), RegisteredSymbols IS shared (the
# provider's symbol is UND) — and yet job 44367535 traced ZERO ibv_* lookups on
# a run whose ibverbs hooks demonstrably fired (26880 sends, ib_mem_region on
# all 8 ranks). Five hypotheses died explaining that: a dlsym-hidden symbol,
# direct linking, a split registry, the wrong NCCL, and symbol versioning.
#
# So this measures rather than guesses, and it is deliberately the dumbest
# possible instrument: PCOMM_TRACE_DLSYM=1 now prints from the FIRST statement of
# dlsym and dlvsym, above init() and above every guard. The previous diagnostic
# printed from inside on_symbol_load, which returns early when real_symbol is
# null — the third time in this project a guard placed for an unrelated reason
# has blinded a diagnostic pointed at something else.
#
# POSITIVE CONTROL: the `<first entry>` line, printed once per function for
# whatever symbol arrives first. dlsym's line proves the instrument ran. An
# earlier probe here mis-stated its own control, so read it in this order:
#
#   neither line          -> the instrument is broken; the finding is not real
#   dlsym only            -> our dlvsym is NOT entered; it becomes a resolution
#                            question, and LD_DEBUG=bindings is finally the right
#                            tool because it finally answers the right question
#   both, dlvsym w/o ibv_ -> NCCL is not reaching dlvsym for these at all
#   both, dlvsym w/ ibv_  -> the loss is downstream, inside on_symbol_load
#
#SBATCH --job-name=pcomm-probe-dlvsym
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

OUT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/probe-dlvsym-${SLURM_JOB_ID:-local}"
rm -rf "$OUT"; mkdir -p "$OUT"

# Same configuration as case 02 — same providers, same NCCL pinning — because a
# probe that changes the workload cannot be compared against the board it is
# explaining. Only the iteration count drops: symbol resolution happens once, at
# NCCL init, so 2 iterations observe exactly what 20 would.
mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x PCOMM_TRACE_DLSYM=1 \
  -x NCCL_DEBUG=WARN -x NCCL_P2P_DISABLE=1 \
  -x NCCL_ALGO=Ring -x NCCL_PROTO=Simple \
  -x NCCL_MIN_NCHANNELS=1 -x NCCL_MAX_NCHANNELS=1 -x NCCL_NTHREADS=256 \
  "$PCOMM" run -p ib -o "$OUT" -- "$APP" 8388608 2 2>&1 | tee "$OUT/run.log"
echo "mpirun exit=${PIPESTATUS[0]}"

echo "=== POSITIVE CONTROL: was each interposer entered at all? ==="
grep -h 'first entry' "$OUT/run.log" | sort -u

echo "=== entry, by interposer and symbol (the answer) ==="
grep -h 'pcomm-entry' "$OUT/run.log" | grep -v 'first entry' | sort -u

echo "=== resolution trace, for contrast with the entry trace above ==="
grep -h 'pcomm-dlsym' "$OUT/run.log" | sort -u

# Counts, so "zero" is stated by the probe rather than inferred from an empty
# section — an earlier probe reported a false positive because grep -c matched
# the filename it was searching rather than a line in it.
echo "=== counts ==="
for pat in 'dlsym <first entry>' 'dlvsym <first entry>' 'dlvsym ' 'dlsym '; do
  printf '%-24s %s\n' "$pat" "$(grep -c -- "$pat" "$OUT/run.log")"
done
