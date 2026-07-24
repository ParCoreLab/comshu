#!/bin/bash
# run_suite.sh — drive the pcomm integration suite on MareNostrum 5.
#
#   run_suite.sh list                    # the cases
#   run_suite.sh submit [case ...]       # sbatch each case (default: all)
#   run_suite.sh check  [case ...]       # aggregate verdicts from finished jobs
#   run_suite.sh status                  # squeue for this suite
#
# Each case's job runs check_capture.py itself and drops result.json in its
# output dir; `check` gathers those into one PASS/FAIL table. Cases 03/04 need
# the gpumem provider (NVBit) built on MN5 — see README.md.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PCOMM_ROOT="${PCOMM_ROOT:-$(cd "$HERE/../../.." && pwd)}"
CASES_DIR="$PCOMM_ROOT/tests/hpc/cases"
OUT_ROOT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}"

all_cases() { for d in "$CASES_DIR"/*/; do basename "$d"; done; }
resolve() { [ "$#" -eq 0 ] && all_cases || printf '%s\n' "$@"; }

cmd="${1:-list}"; shift || true

case "$cmd" in
  list)
    printf '%-20s %-6s %s\n' CASE PROV CHECKS
    for c in $(all_cases); do
      p=$(python3 -c "import json;print(','.join(json.load(open('$CASES_DIR/$c/meta.json'))['providers']))" 2>/dev/null)
      n=$(ls "$CASES_DIR/$c"/checks/*.sql 2>/dev/null | wc -l | tr -d ' ')
      x=$(python3 -c "import json;print('  [xfail]' if 'expect_fail' in json.load(open('$CASES_DIR/$c/meta.json')) else '')" 2>/dev/null)
      printf '%-22s %-6s %s sql%s\n' "$c" "$p" "$n" "$x"
    done ;;

  submit)
    for c in $(resolve "$@"); do
      job="$CASES_DIR/$c/job.sbatch"
      [ -f "$job" ] || { echo "no such case: $c" >&2; continue; }
      id=$(sbatch --parsable "$job")
      echo "submitted $c -> job $id"
    done ;;

  check)
    fail=0
    printf '%-20s %-6s %s\n' CASE VERDICT DETAIL
    for c in $(resolve "$@"); do
      # captures are named by the short label (case dir minus the NN_ prefix),
      # e.g. 02_nccl_internode -> nccl_internode-<jobid>.csv/result.json
      label="${c#*_}"
      d=$(ls -1dt "$OUT_ROOT/$label"-*.csv 2>/dev/null | head -1)
      # A case marked expect_fail in meta.json is xfail: failing/deadlocking is
      # the *expected* state (XFAIL, suite stays green); completing is an alert
      # (XPASS → time to remove the marker and add goldens).
      xfail=$(python3 -c "import json;print('expect_fail' in json.load(open('$CASES_DIR/$c/meta.json')))" 2>/dev/null)
      if [ -z "$d" ] || [ ! -f "$d/result.json" ]; then
        printf '%-20s %-6s %s\n' "$c" "----" "no result yet ($OUT_ROOT/$label-*.csv)"
        [ "$xfail" = "True" ] || fail=1; continue
      fi
      if [ "$xfail" = "True" ]; then
        outcome=$(python3 -c "import json;print(json.load(open('$d/result.json')).get('outcome','?'))")
        if [ "$outcome" = "completed" ]; then
          printf '%-20s %-6s %s\n' "$c" XPASS "unexpectedly ran — remove xfail, add goldens · $(basename "$d")"
        else
          printf '%-20s %-6s %s\n' "$c" XFAIL "known: $outcome (see meta.json) · $(basename "$d")"
        fi
        continue
      fi
      ok=$(python3 -c "import json;print(json.load(open('$d/result.json'))['ok'])")
      n=$(python3 -c "import json;print(json.load(open('$d/result.json')).get('checks',0))")
      if [ "$ok" = "True" ]; then printf '%-20s %-6s %s\n' "$c" PASS "$n checks · $(basename "$d")"
      else printf '%-20s %-6s %s\n' "$c" FAIL "see job .out · $(basename "$d")"; fail=1; fi
    done
    exit $fail ;;

  status) squeue -u "$USER" | tail -n +2 ;;

  *) echo "usage: run_suite.sh {list|submit|check|status} [case ...]" >&2; exit 2 ;;
esac
