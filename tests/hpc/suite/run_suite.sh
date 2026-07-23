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
    printf '%-20s %s\n' CASE PROVIDERS
    for c in $(all_cases); do
      p=$(python3 -c "import json,sys;print(','.join(json.load(open('$CASES_DIR/$c/expect.json'))['providers']))" 2>/dev/null)
      printf '%-20s %s\n' "$c" "$p"
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
      # newest output dir for this case
      d=$(ls -1dt "$OUT_ROOT/$c"-* 2>/dev/null | head -1)
      if [ -z "$d" ] || [ ! -f "$d/result.json" ]; then
        printf '%-20s %-6s %s\n' "$c" "----" "no result yet ($OUT_ROOT/$c-*)"; fail=1; continue
      fi
      ok=$(python3 -c "import json;print(json.load(open('$d/result.json'))['ok'])")
      n=$(python3 -c "import json;d=json.load(open('$d/result.json'));print(sum(1 for x in d['checks'] if not x['ok']))")
      if [ "$ok" = "True" ]; then printf '%-20s %-6s %s\n' "$c" PASS "$(basename "$d")"
      else printf '%-20s %-6s %s\n' "$c" FAIL "$n failed check(s): $(basename "$d")"; fail=1; fi
    done
    exit $fail ;;

  status) squeue -u "$USER" -n \
      pcomm-01-ib-pingpong,pcomm-02-nccl-internode,pcomm-03-nccl-p2p,pcomm-04-nvshmem ;;

  *) echo "usage: run_suite.sh {list|submit|check|status} [case ...]" >&2; exit 2 ;;
esac
