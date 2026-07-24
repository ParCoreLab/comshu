#!/bin/bash
# run_on_mn5.sh — one command, from a checkout of this repo: sync it to
# MareNostrum 5, build everything (tools + ibverbs + gpumem/NVBit), run the whole
# integration suite, and print the board. Blocks until every case has a verdict.
#
#   tests/hpc/run_on_mn5.sh                    # sync + build + run all + check
#   tests/hpc/run_on_mn5.sh --no-build         # skip the rebuild
#   tests/hpc/run_on_mn5.sh --clean            # wipe remote build dirs first
#   tests/hpc/run_on_mn5.sh --cases "01 03"    # a subset (by NN_ prefix or name)
#   tests/hpc/run_on_mn5.sh --check-only       # just re-aggregate existing results
#
# Config via env (defaults are the ParCore MN5 account):
#   PCOMM_REMOTE=koc296797@alogin1.bsc.es     ssh target (key auth assumed)
#   PCOMM_REMOTE_DIR=proj/pcomm               path on the remote (rel. to $HOME)
#   PCOMM_OUT_ROOT=$HOME/pcomm-suite          where captures land (remote)
#
# Notes: the acc_debug QoS allows one submitted job at a time, so cases run
# serially (submit → wait → next); a full run is ~15 min incl. the case 05
# deadlock timeout. Exit code is run_suite's: 0 when all PASS/XFAIL, non-zero on
# any real FAIL or missing result. XFAIL (case 05) does not fail the run.
set -uo pipefail

REMOTE="${PCOMM_REMOTE:-koc296797@alogin1.bsc.es}"
RDIR="${PCOMM_REMOTE_DIR:-proj/pcomm}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SSH="ssh -o BatchMode=yes"

do_build=1 do_clean=0 check_only=0
cases_filter=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)   do_build=0 ;;
    --clean)      do_clean=1 ;;
    --check-only) check_only=1; do_build=0 ;;
    --cases)      cases_filter="$2"; shift ;;
    -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
rsh() { $SSH "$REMOTE" "bash -lc '$*'"; }   # login shell → `module` is available

# --- 1. sync ----------------------------------------------------------------
if [ "$check_only" -eq 0 ]; then
  say "sync $REPO_ROOT → $REMOTE:$RDIR"
  $SSH "$REMOTE" "mkdir -p '$RDIR'"
  rsync -az --delete \
    --exclude '.git' --exclude 'build' --exclude 'build-*' \
    --exclude '.cache' --exclude '*.o' --exclude '*.pcs' \
    "$REPO_ROOT"/ "$REMOTE:$RDIR"/
fi

# --- 2. build ---------------------------------------------------------------
if [ "$do_clean" -eq 1 ]; then
  say "clean remote build dirs"; rsh "cd $RDIR && rm -rf build build-gpumem"
fi
if [ "$do_build" -eq 1 ]; then
  say "build tools + ibverbs provider (gcc-13.2)"
  rsh "module load gcc/13.2.0 cmake >/dev/null 2>&1; cd $RDIR &&
       cmake -B build -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_IBVERBS=ON >/dev/null &&
       cmake --build build -j 2>&1 | tail -2" \
    || { echo 'tools/ibverbs build FAILED' >&2; exit 1; }

  say "build gpumem provider (NVBit, gcc-11.4 host, CUDA C++20, sm_90)"
  rsh "module load gcc/11.4.0 nvidia-hpc-sdk/25.7 cmake >/dev/null 2>&1; cd $RDIR &&
       cmake -B build-gpumem -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_IBVERBS=OFF -DPCOMM_PROVIDER_GPUMEM=ON -DCMAKE_CUDA_ARCHITECTURES=90 -DCMAKE_CUDA_HOST_COMPILER=g++ >/dev/null &&
       cmake --build build-gpumem --target gpumem_trace -j 2>&1 | tail -2 &&
       mkdir -p build/providers/gpumem &&
       cp build-gpumem/providers/gpumem/libgpumem_trace.so build/providers/gpumem/ &&
       ls -la build/providers/*/*.so" \
    || { echo 'gpumem build FAILED' >&2; exit 1; }
fi

# --- 3. run the cases (serial: acc_debug = 1 submitted job at a time) --------
all_cases() { rsh "cd $RDIR && for d in tests/hpc/cases/*/; do basename \$d; done"; }
CASES=$(all_cases)
if [ -n "$cases_filter" ]; then
  sel=""; for f in $cases_filter; do sel="$sel $(echo "$CASES" | grep -E "^${f}(_|\$)" || echo "$f")"; done
  CASES="$sel"
fi

if [ "$check_only" -eq 0 ]; then
  for c in $CASES; do
    say "submit + wait: $c"
    jid=$(rsh "cd $RDIR && sbatch --parsable tests/hpc/cases/$c/job.sbatch") || { echo "submit failed: $c"; continue; }
    echo "  job $jid"
    while :; do
      st=$(rsh "squeue -h -j $jid -o %T 2>/dev/null" | tr -d '[:space:]')
      [ -z "$st" ] && break
      printf '\r  %s: %s   ' "$jid" "$st"; sleep 20
    done
    echo "  done"
  done
fi

# --- 4. board ---------------------------------------------------------------
say "suite board"
rsh "cd $RDIR && PCOMM_OUT_ROOT=\${PCOMM_OUT_ROOT:-\$HOME/pcomm-suite} bash tests/hpc/suite/run_suite.sh check"
rc=$?
echo
[ $rc -eq 0 ] && echo "SUITE GREEN (PASS/XFAIL)" || echo "SUITE RED (exit $rc)"
exit $rc
