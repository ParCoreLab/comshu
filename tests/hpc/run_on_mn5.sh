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
#
# Every verdict is pinned to the job id submitted by THIS run, via a manifest at
# $PCOMM_OUT_ROOT/.last-run.manifest — so a case whose job never produced output
# reports "no result" instead of quietly inheriting the previous run's PASS.
# --check-only re-reads that manifest, and so re-checks that same run.
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

# --- 0. resolve remote paths ------------------------------------------------
# These must be concrete here, not left as remote-side defaults: the job needs
# them exported through sbatch, and `check` needs to look in the same place.
RHOME=$($SSH "$REMOTE" 'echo $HOME' | tr -d '\r') \
  || { echo "cannot reach $REMOTE" >&2; exit 1; }
case "$RDIR" in /*) PROOT="$RDIR" ;; *) PROOT="$RHOME/$RDIR" ;; esac
OUT_ROOT="${PCOMM_OUT_ROOT:-$RHOME/pcomm-suite}"
MANIFEST_REMOTE="$OUT_ROOT/.last-run.manifest"
MANIFEST_LOCAL=$(mktemp)
trap 'rm -f "$MANIFEST_LOCAL"' EXIT

# Wait until a job reaches a terminal state. The point of sacct here is that it
# answers for finished jobs too, so "no longer in squeue" and "the query broke"
# stay distinguishable — the old loop read any empty line as success, meaning a
# dropped ssh silently skipped the wait and moved on to the next case.
wait_job() {
  local jid="$1" st sq errs=0 misses=0
  while :; do
    if ! st=$(rsh "sacct -j $jid -X -n -P -o State 2>/dev/null" | head -1 | tr -d '[:space:]'); then
      errs=$((errs+1))
      [ "$errs" -ge 15 ] && { echo "  !! sacct query failed ${errs}x for $jid — giving up" >&2; return 1; }
      sleep 20; continue
    fi
    errs=0
    case "$st" in
      COMPLETED|FAILED|TIMEOUT|NODE_FAIL|OUT_OF_MEMORY|PREEMPTED|BOOT_FAIL|DEADLINE|REVOKED|CANCELLED*)
        printf '\r  %s: %s            \n' "$jid" "$st"
        # TIMEOUT is how the xfail case reports its deadlock, so it is not an
        # error here — the verdict belongs to `check`, which reads result.json.
        return 0 ;;
      "")
        # sacct can lag for a few seconds after submit; believe squeue first.
        sq=$(rsh "squeue -h -j $jid -o %T 2>/dev/null" | tr -d '[:space:]') || sq=""
        if [ -n "$sq" ]; then misses=0; printf '\r  %s: %s        ' "$jid" "$sq"
        else
          misses=$((misses+1))
          [ "$misses" -ge 5 ] && { echo "  !! $jid unknown to both sacct and squeue" >&2; return 1; }
        fi ;;
      *) misses=0; printf '\r  %s: %s        ' "$jid" "$st" ;;
    esac
    sleep 20
  done
}

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
  sel=""
  for f in $cases_filter; do
    m=$(echo "$CASES" | grep -E "^${f}(_|\$)") \
      || { echo "no case matches '$f'; known: $(echo $CASES)" >&2; exit 2; }
    sel="$sel $m"
  done
  CASES="$sel"
fi

run_rc=0
# Pushed after every submit rather than once at the end, so interrupting a run
# still leaves --check-only reading the right jobs.
push_manifest() {
  $SSH "$REMOTE" "mkdir -p '$OUT_ROOT' && cat > '$MANIFEST_REMOTE'" < "$MANIFEST_LOCAL" \
    || { echo "could not write run manifest" >&2; exit 1; }
}
if [ "$check_only" -eq 0 ]; then
  : > "$MANIFEST_LOCAL"; push_manifest
  for c in $CASES; do
    say "submit + wait: $c"
    # PCOMM_ROOT/PCOMM_OUT_ROOT go through --export so the job honours the same
    # PCOMM_REMOTE_DIR / PCOMM_OUT_ROOT this driver was given, instead of
    # falling back to common.sh's $HOME defaults.
    if ! jid=$(rsh "cd $RDIR && sbatch --parsable --export=ALL,PCOMM_ROOT=$PROOT,PCOMM_OUT_ROOT=$OUT_ROOT tests/hpc/cases/$c/job.sbatch"); then
      echo "  !! submit FAILED: $c" >&2
      # Record it anyway: a case missing from the manifest would fall back to
      # newest-wins in `check` and could inherit an older run's PASS.
      printf '%s %s\n' "$c" SUBMIT_FAILED >> "$MANIFEST_LOCAL"; push_manifest; run_rc=1; continue
    fi
    jid=$(printf '%s' "$jid" | tr -d '[:space:]'); jid="${jid%%;*}"   # "123;cluster"
    if [ -z "$jid" ]; then
      echo "  !! no job id for $c" >&2
      printf '%s %s\n' "$c" SUBMIT_FAILED >> "$MANIFEST_LOCAL"; push_manifest; run_rc=1; continue
    fi
    echo "  job $jid"
    printf '%s %s\n' "$c" "$jid" >> "$MANIFEST_LOCAL"; push_manifest
    wait_job "$jid" || run_rc=1
  done
fi

# --- 4. board ---------------------------------------------------------------
say "suite board"
echo "  results: $OUT_ROOT   manifest: $MANIFEST_REMOTE"
rsh "cd $RDIR && PCOMM_ROOT=$PROOT PCOMM_OUT_ROOT=$OUT_ROOT PCOMM_RUN_MANIFEST=$MANIFEST_REMOTE bash tests/hpc/suite/run_suite.sh check"
rc=$?
[ "$run_rc" -eq 0 ] || rc=$(( rc == 0 ? 1 : rc ))   # a bad submit/wait is a red run
echo
[ $rc -eq 0 ] && echo "SUITE GREEN (PASS/XFAIL)" || echo "SUITE RED (exit $rc)"
exit $rc
