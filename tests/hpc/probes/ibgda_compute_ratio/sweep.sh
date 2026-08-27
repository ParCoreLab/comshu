#!/bin/bash
# The doorbell hijack's overhead as a function of compute:communication ratio.
#
#   sweep.sh                       # the default ladder
#   PCOMM_STEPS="0 2000" sweep.sh  # a subset
#
# Runs on du04 (the only host that can do IBGDA at all — MN5 has no
# PeerMappingOverride and cases 09/10/12 self-skip there). Three configurations
# per compute level, each REPEATED and reported as a median:
#
#   bare     no pcomm            the application's own cost
#   passive  -p mlx5dv           the control plane only; no hijack, no data path
#   hijack   + doorbell hijack   the man-in-the-middle on the UAR page
#
# `passive` is not decoration. Without it a difference between bare and hijack
# could be the hooks, the provider's startup, or the capture's I/O; with it, the
# hijack column is attributable to the hijack.
#
# MEDIAN OF N, not the mean and not the minimum. du04 has no scheduler and other
# people's jobs live on it, so a single run is at the mercy of whatever else is
# resident; the mean is dragged by one bad sample and the minimum flatters
# whichever configuration got the quietest slot.
set -u
ROOT="${PCOMM_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)}"
# shellcheck source=/dev/null
. "$ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
MPIRUN="${PCOMM_MPIRUN:-mpirun}"
APP="${PCOMM_IBGDA_APP:-$HOME/stencil_putget}"
OUT="${PCOMM_OUT_ROOT:-/tmp/pcomm-ratio-$$}"
ELEMS="${PCOMM_ELEMS:-1024}"
ITERS="${PCOMM_ITERS:-200}"
REPS="${PCOMM_REPS:-3}"
# The ladder. 0 reproduces 04_nvshmem/putget.cu's shape exactly, which is what
# makes the published +68%/+86% comparable to everything to the right of it.
STEPS="${PCOMM_STEPS:-0 200 2000 20000}"

mkdir -p "$OUT"

# GPU-INITIATED IB, and this is copied from 12_ibgda_doorbell/job.sbatch rather
# than reinvented. All five matter: without REMOTE_TRANSPORT=ibgda and
# NIC_HANDLER=gpu the sends go through the host proxy, no doorbell is ever rung
# from the device, and the hijack would measure as free because it never
# engaged. The same env goes to every configuration -- a difference in the
# transport between columns would make the whole table meaningless.
export NVSHMEM_BOOTSTRAP=MPI
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=gpu
export NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem

NV_ENV="-x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P -x NVSHMEM_REMOTE_TRANSPORT
        -x NVSHMEM_IB_ENABLE_IBGDA -x NVSHMEM_IBGDA_NIC_HANDLER
        -x NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE
        -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH"

# Wall clock of one run, in ms, from the application's OWN cuda events -- not
# from `time`. Process startup, NVSHMEM bootstrap and the provider's own
# initialisation are real costs but they are PER RUN, not per doorbell, and
# including them would dilute exactly the effect being measured.
run_once() {   # <mode> <steps> -> ms on stdout, or empty on failure
  local mode="$1" steps="$2" line=""
  case "$mode" in
    bare)    line=$(timeout 300 $MPIRUN -np 2 $NV_ENV \
                      "$APP" "$ELEMS" "$ITERS" "$steps" 2>/dev/null) ;;
    passive) line=$(timeout 300 $MPIRUN -np 2 $NV_ENV "$PCOMM" run -p mlx5dv \
                      -o "$OUT/passive" -- "$APP" "$ELEMS" "$ITERS" "$steps" 2>/dev/null) ;;
    hijack)  line=$(timeout 300 $MPIRUN -np 2 $NV_ENV "$PCOMM" run -p mlx5dv \
                      --enable-doorbell-hijack \
                      -o "$OUT/hijack" -- "$APP" "$ELEMS" "$ITERS" "$steps" 2>/dev/null) ;;
  esac
  # VERIFY OK is a precondition for the number being meaningful at all: the
  # hijack owns the doorbell, so a run that moved the wrong bytes must not
  # contribute a timing sample.
  echo "$line" | grep -q "VERIFY FAIL" && { echo "" ; return; }
  echo "$line" | grep -q "VERIFY OK"   || { echo "" ; return; }
  echo "$line" | sed -n 's/.* ms=\([0-9.]*\) .*/\1/p' | sort -g | tail -1
}

median() { printf '%s\n' "$@" | sort -g | awk '{a[NR]=$1} END{if(NR)print a[int((NR+1)/2)]}'; }

printf '%8s  %10s  %10s  %10s  %8s  %8s\n' steps bare_ms passive_ms hijack_ms "pass%" "hijack%"
for s in $STEPS; do
  declare -a b=() p=() h=()
  for _ in $(seq "$REPS"); do
    v=$(run_once bare "$s");    [ -n "$v" ] && b+=("$v")
    v=$(run_once passive "$s"); [ -n "$v" ] && p+=("$v")
    v=$(run_once hijack "$s");  [ -n "$v" ] && h+=("$v")
  done
  # An empty column is REPORTED, never silently skipped or filled with a zero:
  # "this configuration did not complete" is a result about the hijack.
  bm=$(median "${b[@]:-}"); pm=$(median "${p[@]:-}"); hm=$(median "${h[@]:-}")
  if [ -n "$bm" ] && [ -n "$pm" ] && [ -n "$hm" ]; then
    printf '%8s  %10s  %10s  %10s  %7.0f%%  %7.0f%%\n' "$s" "$bm" "$pm" "$hm" \
      "$(echo "$pm $bm" | awk '{print ($1/$2-1)*100}')" \
      "$(echo "$hm $bm" | awk '{print ($1/$2-1)*100}')"
  else
    printf '%8s  %10s  %10s  %10s  %8s  %8s   (a configuration did not complete)\n' \
      "$s" "${bm:--}" "${pm:--}" "${hm:--}" - -
  fi
done

echo
echo "doorbells forwarded in the last hijack run:"
"$PCOMM" read "$OUT/hijack" 2>/dev/null | grep -E "21[24]\.pcs" || echo "  (none)"
