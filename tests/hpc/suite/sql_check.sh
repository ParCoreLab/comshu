#!/bin/bash
# sql_check.sh — run a case's SQL checks against a pcomm-read CSV export.
#
#   sql_check.sh <csv-dir> <case-dir> [--update]
#
# The capture is loaded into an in-memory relational form (one table per
# <name>.csv, columns from the header row) and two *different kinds* of check
# are run against it. The distinction is the point of this script:
#
#   checks/<name>.sql   GOLDEN — output diffed against checks/<name>.golden.
#                       A characterization test: "the tool still does what it
#                       did". It is the regression net, and it can only ever
#                       report *change*, never correctness.
#   asserts/<name>.sql  ASSERTION — returns the OFFENDING ROWS; empty is a pass.
#                       The query is the specification, so a failure prints what
#                       violated it rather than a diff of two opaque blobs.
#
# Why both: a golden cannot catch a bug that predates it. 04_nvshmem's
# peer_fanout golden asserted `peers = 2` for a ring in which each PE stores to
# exactly one neighbour — it had enshrined the NVSHMEM attribution bug, and was
# green. A golden also breaks on things that were never the claim: the fabric
# handed both ranks of 01_ib_pingpong `qp_num 449` (job 44111484) and the
# topology golden went red with nothing wrong in the tool.
#
# An assertion earns its keep only if it is derived rather than observed. The
# rule, enforced by review: **every assertion must either reference `params`, or
# be an invariant that names no specific rank, device or QP number.** An
# assertion that re-encodes today's output in SQL is a golden wearing a costume.
#
# `params` is materialised from meta.json's "workload" block — the oracle's
# inputs (ranks, iters, message size, ...), which were always sitting in
# job.sbatch and had simply never been written down as an oracle. So an
# assertion states its own derivation:
#
#   SELECT rank, count(*) AS n FROM ib_post_send GROUP BY rank
#   HAVING n != (SELECT iters FROM params);
#
# Assertion source is suite/asserts/*.sql (cross-cutting, run for every case
# whose capture has the tables) plus <case>/asserts/*.sql. A file may declare
#   -- requires: gpu_access params.gpus
# and is reported [skip] when a named table is absent/empty or a named param is
# null — the capture simply cannot speak to it.
#
# meta.json's "expect_fail_asserts" marks assertions that are known to fail
# against a live bug, with the reason. Those report [xfail] and do not fail the
# case; if one starts passing it reports [XPASS] loudly (remove the marker) but
# does not turn the board red — some of them fail only on the runs where the
# underlying corruption actually occurs.
#
# Goldens must be deterministic (aggregate away ticks, qp_num/LID values,
# addresses, wr_id) and ORDER BY their output. --update regenerates goldens from
# this run and never touches assertions — an assertion has nothing to regenerate,
# which is most of why it is the better instrument.
#
# Engine is sqlite3 (present everywhere, zero setup); queries are kept portable
# ANSI SQL so DuckDB can be dropped in by changing SQL_ENGINE.
set -u
CSV_DIR="${1:?usage: sql_check.sh <csv-dir> <case-dir> [--update]}"
CASE_DIR="${2:?usage: sql_check.sh <csv-dir> <case-dir> [--update]}"
UPDATE="${3:-}"
SQL_ENGINE="${SQL_ENGINE:-sqlite3}"
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DB="$(mktemp)"; trap 'rm -f "$DB"' EXIT
# Load every CSV as a table named after the file (sqlite auto-creates columns
# from the header row when the table does not yet exist; values arrive as TEXT,
# queries CAST what they aggregate).
{
  echo ".mode csv"
  for f in "$CSV_DIR"/*.csv; do
    [ -s "$f" ] || continue                       # skip empty tables
    echo ".import $f $(basename "$f" .csv)"
  done
} | "$SQL_ENGINE" "$DB" 2>/dev/null

case_name="$(basename "$CASE_DIR")"
meta="$CASE_DIR/meta.json"
checks_dir="$CASE_DIR/checks"
[ -d "$checks_dir" ] || { echo "$case_name: no checks/ dir" >&2; exit 2; }

sql() { "$SQL_ENGINE" "$DB" "$@"; }
have_table() { [ -n "$(sql "SELECT name FROM sqlite_master WHERE type IN ('table','view') AND name='$1';")" ]; }
row_count()  { sql "SELECT count(*) FROM \"$1\";" 2>/dev/null; }

# ---------------------------------------------------------------- params -----
# The workload block is a flat object of integers (or null). The column list is
# fixed and shared by every case so an assertion referencing a param a case does
# not define gets NULL — which `-- requires:` turns into a visible [skip] rather
# than a query that quietly matches nothing and reads as a pass.
PARAM_COLS="ranks nodes gpus iters msg_bytes elems elem_bytes regions_per_rank"
meta_py() { python3 - "$meta" "$PARAM_COLS" "$@"; }

params_sql="$(meta_py <<'PY'
import json, sys
meta, cols = sys.argv[1], sys.argv[2].split()
w = json.load(open(meta)).get("workload", {})
unknown = [k for k in w if k not in cols]
if unknown:
    sys.exit("unknown workload key(s) %s — allowed: %s" % (unknown, cols))
print("CREATE TABLE params (%s);" % ", ".join("%s INTEGER" % c for c in cols))
vals = ["NULL" if w.get(c) is None else str(int(w[c])) for c in cols]
print("INSERT INTO params VALUES (%s);" % ", ".join(vals))
PY
2>&1)" || { echo "$case_name: bad workload block in meta.json: $params_sql" >&2; exit 2; }
sql "$params_sql"

# A rank appears in whichever tables the capture happens to have, so the
# cross-cutting "did every rank record anything" assertion needs one place to
# look. Built from the imported tables that actually carry a rank column.
{
  rank_tabs=()
  for t in $(sql "SELECT name FROM sqlite_master WHERE type='table' AND name!='params';"); do
    sql "SELECT rank FROM \"$t\" LIMIT 0;" >/dev/null 2>&1 && rank_tabs+=("$t")
  done
  if [ "${#rank_tabs[@]}" -gt 0 ]; then
    q="CREATE VIEW ranks_seen AS SELECT DISTINCT CAST(rank AS INTEGER) AS rank FROM \"${rank_tabs[0]}\""
    for t in "${rank_tabs[@]:1}"; do q="$q UNION SELECT DISTINCT CAST(rank AS INTEGER) FROM \"$t\""; done
    sql "$q;"
  fi
}

# --------------------------------------------------------------- goldens -----
gfail=0; gn=0
echo "  -- goldens (regression net: reports change, not correctness)"
for q in "$checks_dir"/*.sql; do
  [ -e "$q" ] || { echo "$case_name: no .sql checks" >&2; exit 2; }
  name="$(basename "$q" .sql)"
  golden="$checks_dir/$name.golden"
  got="$("$SQL_ENGINE" -csv -header "$DB" < "$q" 2>&1)"
  gn=$((gn+1))
  if [ "$UPDATE" = "--update" ]; then
    printf '%s\n' "$got" > "$golden"; echo "  [updated] $name"; continue
  fi
  if [ ! -f "$golden" ]; then
    echo "  [MISS] $name — no golden (run with --update once verified)"; gfail=1; continue
  fi
  if diff -q <(printf '%s\n' "$got") "$golden" >/dev/null; then
    echo "  [ok ] $name"
  else
    echo "  [XX ] $name — differs from golden:"
    diff <(printf '%s\n' "$got") "$golden" | sed 's/^/        /'
    gfail=1
  fi
done

if [ "$UPDATE" = "--update" ]; then echo "$case_name: $gn golden(s) written"; exit 0; fi

# ------------------------------------------------------------ assertions -----
afail=0; an=0; askip=0; axfail=0; axpass=0

# expect_fail_asserts: { "<assert name>": "<why, ideally a plan reference>" }
# The reasons are long by design — they are where the evidence and the blocking
# plan live — so the board prints the first sentence and points at meta.json for
# the rest. A verdict line nobody reads is not better than no verdict line.
xfail_reason() {
  python3 - "$meta" "$1" <<'PY'
import json, sys
why = json.load(open(sys.argv[1])).get("expect_fail_asserts", {}).get(sys.argv[2], "")
if len(why) > 150:
    cut = why.find(". ")
    why = (why[:cut + 1] if 40 < cut < 150 else why[:150] + "...") + " (meta.json)"
print(why)
PY
}

# "Every expected table is non-empty" is not written as SQL because the failure
# it guards against is SQL not running at all: this suite has twice reported
# STABLE on a `no such table` error, a capture that was empty for an
# environmental reason silently passing every check that found nothing to
# contradict. meta.json lists what the workload must have produced.
echo "  -- assertions (derived from the workload; empty result = pass)"
expect_tables="$(python3 - "$meta" <<'PY'
import json, sys
print(" ".join(json.load(open(sys.argv[1])).get("expect_tables", [])))
PY
)"
if [ -n "$expect_tables" ]; then
  an=$((an+1)); missing=""
  for t in $expect_tables; do
    if ! have_table "$t"; then missing="$missing $t(absent)"
    elif [ "$(row_count "$t")" = "0" ]; then missing="$missing $t(empty)"; fi
  done
  why="$(xfail_reason tables_present)"
  if [ -n "$missing" ] && [ -n "$why" ]; then
    echo "  [xfail] tables_present —$missing; known: $why"; axfail=$((axfail+1))
  elif [ -n "$missing" ]; then
    echo "  [XX ] tables_present — the workload must produce these and did not:$missing"
    afail=1
  elif [ -n "$why" ]; then
    echo "  [XPASS] tables_present — marked expect_fail but passes; drop the marker in meta.json"
    axpass=$((axpass+1))
  else
    echo "  [ok ] tables_present"
  fi
fi

run_assert() {
  q="$1"; name="$(basename "$q" .sql)"
  # -- requires: <table|params.<col>> ...  — the capture cannot speak to this
  need="$(sed -n 's/^-- requires:[[:space:]]*//p' "$q" | tr '\n' ' ')"
  for r in $need; do
    case "$r" in
      params.*) col="${r#params.}"
        if [ "$(sql "SELECT count(*) FROM params WHERE $col IS NOT NULL;")" = "0" ]; then
          echo "  [skip] $name — workload has no $col"; askip=$((askip+1)); return; fi ;;
      *) if ! have_table "$r" || [ "$(row_count "$r")" = "0" ]; then
           echo "  [skip] $name — no $r rows in this capture"; askip=$((askip+1)); return; fi ;;
    esac
  done
  an=$((an+1))
  out="$("$SQL_ENGINE" -csv -header "$DB" < "$q" 2>&1)"
  why="$(xfail_reason "$name")"
  if [ -z "$out" ]; then
    if [ -n "$why" ]; then
      echo "  [XPASS] $name — marked expect_fail but passes; drop the marker in meta.json"
      axpass=$((axpass+1))
    else
      echo "  [ok ] $name"
    fi
  elif [ -n "$why" ]; then
    echo "  [xfail] $name — known: $why"
    printf '%s\n' "$out" | head -12 | sed 's/^/        /'
    axfail=$((axfail+1))
  else
    echo "  [XX ] $name — violated by:"
    printf '%s\n' "$out" | head -25 | sed 's/^/        /'
    afail=1
  fi
}

for q in "$SUITE_DIR"/asserts/*.sql "$CASE_DIR"/asserts/*.sql; do
  [ -e "$q" ] || continue
  run_assert "$q"
done

# ------------------------------------------------------------------ verdict --
fail=$(( gfail || afail ))
n=$((gn + an))
if [ "$fail" -eq 0 ]; then echo "=== $case_name : PASS ($gn goldens, $an assertions) ==="
else echo "=== $case_name : FAIL (goldens $([ $gfail -eq 0 ] && echo ok || echo FAILED), assertions $([ $afail -eq 0 ] && echo ok || echo FAILED)) ==="; fi
[ "$axpass" -gt 0 ] && echo "  !! $axpass assertion(s) marked expect_fail now pass — update meta.json"

# Machine-readable verdict for the aggregator. `ok`/`checks` keep their old
# meaning so run_suite.sh is unaffected; the two verdicts are reported
# separately because they mean different things — a failed assertion is a
# correctness bug, a failed golden is a change that may or may not be one.
printf '{"case":"%s","ok":%s,"checks":%d,"goldens":{"n":%d,"ok":%s},"asserts":{"n":%d,"ok":%s,"skipped":%d,"xfail":%d,"xpass":%d}}\n' \
       "$case_name" "$([ $fail -eq 0 ] && echo true || echo false)" "$n" \
       "$gn" "$([ $gfail -eq 0 ] && echo true || echo false)" \
       "$an" "$([ $afail -eq 0 ] && echo true || echo false)" \
       "$askip" "$axfail" "$axpass" > "$CSV_DIR/result.json"
exit $fail
