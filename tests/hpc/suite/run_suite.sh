#!/bin/bash
# run_suite.sh — drive the comshu integration suite on MareNostrum 5.
#
#   run_suite.sh list                    # the cases
#   run_suite.sh submit [case ...]       # sbatch each case (default: all)
#   run_suite.sh check  [case ...]       # aggregate verdicts from finished jobs
#   run_suite.sh status                  # squeue for this suite
#
# Each case's job runs sql_check.sh itself and drops result.json in its
# output dir; `check` gathers those into one PASS/FAIL table. Cases 03/04 need
# the gpumem provider (NVBit) built on MN5 — see README.md.
#
# Env: COMSHU_OUT_ROOT     where captures live (default $HOME/comshu-suite)
#      COMSHU_RUN_MANIFEST "<case> <jobid>" table pinning each verdict to the job
#                         from that run, so a dead job can't inherit an older
#                         run's PASS. run_on_mn5.sh writes one automatically.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMSHU_ROOT="${COMSHU_ROOT:-$(cd "$HERE/../../.." && pwd)}"
CASES_DIR="$COMSHU_ROOT/tests/hpc/cases"
OUT_ROOT="${COMSHU_OUT_ROOT:-$HOME/comshu-suite}"

all_cases() { for d in "$CASES_DIR"/*/; do basename "$d"; done; }
resolve() { [ "$#" -eq 0 ] && all_cases || printf '%s\n' "$@"; }

# A run manifest ("<case> <jobid>" per line, written by run_on_mn5.sh) pins each
# verdict to the job that was actually submitted for THIS run. Without it we can
# only take the newest capture on disk, which silently reports an older run's
# PASS when a job dies before writing anything. Cases absent from the manifest —
# or a bare `check` with no manifest at all — keep the newest-wins behaviour.
MANIFEST="${COMSHU_RUN_MANIFEST:-}"
manifest_jid() {
  [ -n "$MANIFEST" ] && [ -f "$MANIFEST" ] || return 0
  awk -v c="$1" '$1==c{print $2; exit}' "$MANIFEST"
}

cmd="${1:-list}"; shift || true

case "$cmd" in
  list)
    # Assertions are listed first because they are the primary check: derived
    # from the workload, so a green one means correct rather than unchanged.
    # A case with 0 of them is only characterized, not tested — see README's
    # "Adding a case".
    printf '%-22s %-6s %s\n' CASE PROV CHECKS
    for c in $(all_cases); do
      p=$(python3 -c "import json;print(','.join(json.load(open('$CASES_DIR/$c/meta.json'))['providers']))" 2>/dev/null)
      g=$(ls "$CASES_DIR/$c"/checks/*.sql 2>/dev/null | wc -l | tr -d ' ')
      a=$(ls "$CASES_DIR/$c"/asserts/*.sql 2>/dev/null | wc -l | tr -d ' ')
      x=$(python3 -c "import json;print('  [xfail]' if 'expect_fail' in json.load(open('$CASES_DIR/$c/meta.json')) else '')" 2>/dev/null)
      printf '%-22s %-6s %s asserts + %s goldens%s\n' "$c" "$p" "$a" "$g" "$x"
    done ;;

  submit)
    for c in $(resolve "$@"); do
      job="$CASES_DIR/$c/job.sbatch"
      [ -f "$job" ] || { echo "no such case: $c" >&2; continue; }
      id=$(sbatch --parsable "$job")
      echo "submitted $c -> job $id"
    done ;;

  check)
    fail=0 skipped=0
    printf '%-20s %-6s %s\n' CASE VERDICT DETAIL
    for c in $(resolve "$@"); do
      # captures are named by the short label (case dir minus the NN_ prefix),
      # e.g. 02_nccl_internode -> nccl_internode-<jobid>.csv/result.json
      label="${c#*_}"
      jid=$(manifest_jid "$c")
      stale=""   # set below when this verdict did not come from this run
      # SKIPPED is written by run_on_mn5.sh --quick. It is not a pass and not a
      # failure: the case was deliberately not run, so the run is GREEN but
      # INCOMPLETE, and the board has to say so rather than leaving a blank that
      # reads like coverage.
      if [ "$jid" = "SKIPPED" ]; then
        printf '%-20s %-6s %s\n' "$c" SKIP "not run (--quick) — this path went unchecked"
        skipped=$((skipped+1)); continue
      fi
      if [ -n "$jid" ]; then
        # pinned: only this job's capture counts (jid may be a sentinel like
        # SUBMIT_FAILED, which simply never matches a directory → no result)
        d="$OUT_ROOT/$label-$jid.csv"; [ -d "$d" ] || d=""
        want="$OUT_ROOT/$label-$jid.csv (job $jid)"
      else
        d=$(ls -1dt "$OUT_ROOT/$label"-*.csv 2>/dev/null | head -1)
        want="$OUT_ROOT/$label-*.csv"
        # NOT FROM THIS RUN, and the board has to say so. Newest-wins is the
        # right fallback — a bare `check` has no manifest and must still report
        # something — but rendering an unpinned verdict identically to a pinned
        # one presents an unrelated job as this run's result.
        #
        # It is not hypothetical. On 2026-08-17 the board showed
        # `05_nccl_p2p FAIL` beside seven cases that had just run, and the
        # verdict came from job 44699869: an ad-hoc probe of the cuMemCreate
        # hook that was REVERTED in a05aa28. Its objects golden failed because
        # that experiment recorded CUDA's 2 MiB pool chunks as application
        # objects — 8 per rank against the expected 1. Code that no longer
        # exists, reported as a failure of HEAD, and it cost a real
        # investigation to find that out.
        stale=" [stale: not in this run]"
      fi
      # A case marked expect_fail in meta.json is xfail: failing/deadlocking is
      # the *expected* state (XFAIL, suite stays green); completing is an alert
      # (XPASS → time to remove the marker and add goldens).
      xfail=$(python3 -c "import json;print('expect_fail' in json.load(open('$CASES_DIR/$c/meta.json')))" 2>/dev/null)
      if [ -z "$d" ] || [ ! -f "$d/result.json" ]; then
        printf '%-20s %-6s %s\n' "$c" "----" "no result ($want)"
        [ "$xfail" = "True" ] || fail=1; continue
      fi
      if [ "$xfail" = "True" ]; then
        # Keyed on `ok`, which every writer of a result.json actually sets —
        # sql_check.sh and each case's failure path both do. This read `outcome`,
        # which NOTHING writes, so it was always '?': the XFAIL branch was taken
        # unconditionally and XPASS was unreachable. A case marked expect_fail
        # that started working would have gone on reporting XFAIL forever, which
        # is precisely the signal the marker exists to produce. Latent rather
        # than live — no case is currently case-level xfail — but the whole point
        # of the mechanism is to notice, and it could not.
        outcome=$(python3 -c "import json;d=json.load(open('$d/result.json'));print('completed' if d.get('ok') and not d.get('skipped') else d.get('reason') or 'did not complete')")
        if [ "$outcome" = "completed" ]; then
          printf '%-20s %-6s %s\n' "$c" XPASS "unexpectedly ran — remove xfail, add goldens · $(basename "$d")"
        else
          printf '%-20s %-6s %s\n' "$c" XFAIL "known: $outcome (see meta.json) · $(basename "$d")"
        fi
        continue
      fi
      ok=$(python3 -c "import json;print(json.load(open('$d/result.json'))['ok'])")
      n=$(python3 -c "import json;print(json.load(open('$d/result.json')).get('checks',0))")
      # A case that declined to run is not a case that passed. It sets ok:true
      # so the board stays green — the host genuinely cannot test it, and a red
      # square there would report a fact about the cluster as a defect — but
      # rendering it as PASS tells the reader something untrue. Case 09 skips on
      # MN5, which has no IBGDA, and showed up as "PASS 0 checks" in 0s among
      # eight real passes: nine greens implying coverage that does not exist.
      why=$(python3 -c "import json;d=json.load(open('$d/result.json'));print(d.get('reason','') if d.get('skipped') else '')")
      # Assertion-level xfails ride inside a PASS. Case 08 passes with one
      # known-failing assertion (perftest's mlx5dv_create_qp, unhooked by the ib
      # provider) and the board said only "PASS 10 checks" — true, and quiet
      # about the part that is known broken.
      nx=$(python3 -c "import json;print(json.load(open('$d/result.json')).get('asserts',{}).get('xfail',0))" 2>/dev/null || echo 0)
      if [ -n "$why" ]; then
        printf '%-20s %-6s %s\n' "$c" SKIP "$why · $(basename "$d")$stale"
      elif [ "$ok" = "True" ]; then
        printf '%-20s %-6s %s\n' "$c" PASS "$n checks${nx:+$([ "$nx" -gt 0 ] && echo ", $nx xfail")} · $(basename "$d")$stale"
      else printf '%-20s %-6s %s\n' "$c" FAIL "see job .out · $(basename "$d")$stale"; fail=1; fi
    done
    [ "$skipped" -gt 0 ] && echo "  !! INCOMPLETE: $skipped case(s) skipped — a green board here covers less than a full run"
    exit $fail ;;

  status) squeue -u "$USER" | tail -n +2 ;;

  *) echo "usage: run_suite.sh {list|submit|check|status} [case ...]" >&2; exit 2 ;;
esac
