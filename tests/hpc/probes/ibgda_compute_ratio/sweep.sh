#!/bin/bash
# The doorbell hijack's overhead as a function of compute:communication ratio.
#
#   sweep.sh                       # the default ladder
#   COMSHU_STEPS="0 2000" sweep.sh  # a subset
#
# Runs on du04 (the only host that can do IBGDA at all — MN5 has no
# PeerMappingOverride and cases 09/10/12 self-skip there). Three configurations
# per compute level, each REPEATED and reported as a median:
#
#   bare     no comshu            the application's own cost
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
# du04's login locale is tr_TR, where printf's %f expects a decimal COMMA while
# awk emits a dot -- so every percentage printed 'invalid number' beside a row
# that was otherwise correct. Numbers in a measurement harness are C numbers.
export LC_ALL=C
ROOT="${COMSHU_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)}"
# shellcheck source=/dev/null
. "$ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime
MPIRUN="${COMSHU_MPIRUN:-mpirun}"
# NVSHMEM's own libraries. du04 has no `module` and no ld.so.conf entry for the
# HPC SDK, so a workload linked against libnvshmem_host.so.3 does not start at
# all: "error while loading shared libraries". The first sweep reported four
# rows of "did not complete" for exactly this and named no cause, because the
# runs were piped to /dev/null -- see the note on run_once.
[ -n "${NVSHMEM_HOME:-}" ] && [ -d "$NVSHMEM_HOME/lib" ] \
  && export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:${LD_LIBRARY_PATH:-}"
APP="${COMSHU_IBGDA_APP:-$HOME/stencil_putget}"
OUT="${COMSHU_OUT_ROOT:-/tmp/comshu-ratio-$$}"
ELEMS="${COMSHU_ELEMS:-1024}"
ITERS="${COMSHU_ITERS:-200}"
REPS="${COMSHU_REPS:-3}"
# The ladder. 0 reproduces 04_nvshmem/putget.cu's shape exactly, which is what
# makes the published +68%/+86% comparable to everything to the right of it.
STEPS="${COMSHU_STEPS:-0 200 2000 20000}"
# The fourth column separates the two costs inside the hijack: `fwd` forwards
# the doorbell and records it but skips the WQE copy-out. If fwd is close to
# bare, the cost is the decode and batching fixes it; if fwd is close to
# hijack, the cost is the host round trip and only the on-GPU repeater does.
# That is the whole Stage-3 decision, and it is one column.

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
        -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH"

# STDERR IS KEPT, and this is the second time in this session that hiding it
# cost a run. A `2>/dev/null` here turned four failed configurations into four
# rows of "did not complete" with the cause nowhere: the workload could not
# find libnvshmem_host.so.3 and said so on a stream nobody was reading.
#
# Wall clock of one run, in ms, from the application's OWN cuda events -- not
# from `time`. Process startup, NVSHMEM bootstrap and the provider's own
# initialisation are real costs but they are PER RUN, not per doorbell, and
# including them would dilute exactly the effect being measured.
run_once() {   # <mode> <steps> -> ms on stdout, or empty on failure
  local mode="$1" steps="$2" line=""
  case "$mode" in
    bare)    line=$(timeout 300 $MPIRUN -np 2 $NV_ENV \
                      "$APP" "$ELEMS" "$ITERS" "$steps" 2>>"$OUT/fail.log") ;;
    passive) line=$(timeout 300 $MPIRUN -np 2 $NV_ENV "$COMSHU" run -p mlx5dv \
                      -o "$OUT/passive" -- "$APP" "$ELEMS" "$ITERS" "$steps" 2>>"$OUT/fail.log") ;;
    hijack)  line=$(timeout 300 $MPIRUN -np 2 $NV_ENV "$COMSHU" run -p mlx5dv \
                      --enable-doorbell-hijack \
                      -o "$OUT/hijack" -- "$APP" "$ELEMS" "$ITERS" "$steps" 2>>"$OUT/fail.log") ;;
    # -x VAR=VALUE, set INLINE and never exported into this shell. Exporting it
    # and passing a bare `-x COMSHU_HIJACK_NO_DECODE` put it in the environment of
    # the `hijack` runs too, so both columns measured the same configuration and
    # the decode looked like it cost 5%. The absent 214.pcs stream in the hijack
    # capture is what gave it away -- a hijack run that emits no Mlx5DeviceSend
    # has not decoded anything.
    fwd)     line=$(timeout 300 $MPIRUN -np 2 $NV_ENV -x COMSHU_HIJACK_NO_DECODE=1 \
                      "$COMSHU" run -p mlx5dv --enable-doorbell-hijack \
                      -o "$OUT/fwd" -- "$APP" "$ELEMS" "$ITERS" "$steps" 2>>"$OUT/fail.log") ;;
  esac
  # VERIFY OK is a precondition for the number being meaningful at all: the
  # hijack owns the doorbell, so a run that moved the wrong bytes must not
  # contribute a timing sample.
  echo "$line" | grep -q "VERIFY FAIL" && { echo "" ; return; }
  echo "$line" | grep -q "VERIFY OK"   || { echo "" ; return; }
  echo "$line" | sed -n 's/.* ms=\([0-9.]*\) .*/\1/p' | sort -g | tail -1
}

median() { printf '%s\n' "$@" | sort -g | awk '{a[NR]=$1} END{if(NR)print a[int((NR+1)/2)]}'; }

printf '%8s  %10s  %10s  %10s  %10s  %9s  %9s\n' \
  steps bare_ms passive_ms fwd_ms hijack_ms "fwd x" "hijack x"
for s in $STEPS; do
  declare -a b=() p=() h=() f=()
  for _ in $(seq "$REPS"); do
    v=$(run_once bare "$s");    [ -n "$v" ] && b+=("$v")
    v=$(run_once passive "$s"); [ -n "$v" ] && p+=("$v")
    v=$(run_once fwd "$s");     [ -n "$v" ] && f+=("$v")
    v=$(run_once hijack "$s");  [ -n "$v" ] && h+=("$v")
  done
  # An empty column is REPORTED, never silently skipped or filled with a zero:
  # "this configuration did not complete" is a result about the hijack.
  bm=$(median "${b[@]:-}"); pm=$(median "${p[@]:-}")
  hm=$(median "${h[@]:-}"); fm=$(median "${f[@]:-}")
  # A MULTIPLE, not a percentage. The published figures were +68%/+86% of WALL
  # CLOCK on a run whose NVSHMEM startup was ~3.5 s of a ~3.5 s run; this timer
  # is the application's own CUDA events around the communication loop alone, so
  # the ratios here are large and are not comparable to those percentages. Saying
  # "133x" rather than "13184%" is the honest way to write a number that big.
  if [ -n "$bm" ] && [ -n "$pm" ] && [ -n "$hm" ] && [ -n "$fm" ]; then
    printf '%8s  %10s  %10s  %10s  %10s  %8.1fx  %8.1fx\n' "$s" "$bm" "$pm" "$fm" "$hm" \
      "$(echo "$fm $bm" | awk '{print $1/$2}')" \
      "$(echo "$hm $bm" | awk '{print $1/$2}')"
  else
    printf '%8s  %10s  %10s  %10s  %10s  %9s  %9s   (did not complete: %s)\n' \
      "$s" "${bm:--}" "${pm:--}" "${fm:--}" "${hm:--}" - - \
      "$(grep -m1 -iE 'error|cannot|abort' "$OUT/fail.log" 2>/dev/null | cut -c1-90)"
  fi
done

# THE CONFIGURATIONS MUST BE DIFFERENT, and this checks it from the CAPTURE
# rather than from the environment that was supposed to produce it. `fwd` must
# have doorbell rows (212) and no send rows (214); `hijack` must have both. If
# they look alike, the two columns measured one thing and the table is void.
echo
for m in fwd hijack; do
  db=$("$COMSHU" read "$OUT/$m" 2>/dev/null | awk '$2==212 {n+=$5} END{print n+0}')
  sd=$("$COMSHU" read "$OUT/$m" 2>/dev/null | awk '$2==214 {n+=$5} END{print n+0}')
  echo "$m: $db doorbells, $sd device sends"
done
echo "(fwd must show 0 device sends and hijack must show many; equal columns mean"
echo " the two configurations were the same run and the table above is void)"

echo
echo "doorbells forwarded in the last hijack run:"
"$COMSHU" read "$OUT/hijack" 2>/dev/null | grep -E "21[24]\.pcs" || echo "  (none)"
