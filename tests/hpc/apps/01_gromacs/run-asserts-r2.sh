#!/bin/bash
# run-asserts-r2.sh — run asserts-r2/*.sql against a DERIVED (parquet) export.
#
#   run-asserts-r2.sh <export.pq> <case-dir> [variant]
#
# WHY THIS EXISTS AND ROUND 1's asserts.sh DOES NOT COVER IT.
#
# Round 1 had to bring a runner because suite/sql_check.sh builds `params` from
# a hard-coded fifteen-name column list and rejects npp/npme/natoms — that is
# NOTES.md's F5 and it still stands. Round 2 hits the SECOND half of the same
# problem, which round 1 never met because it never queried a derived table:
#
#   * round 1's asserts.sh loads a CSV export into **sqlite3**;
#   * `pcomm export --csv` emits one CSV per RECORD TYPE — the raw 600s and 800s
#     and nothing else;
#   * transport_mix, tag_flow, tag_recv_resolved and link_health are built by
#     `pcomm link` and live only in the **parquet** export.
#
# So the four tables this round exists to test cannot be reached by the runner
# this case already ships, and there is no documented runner that can reach them
# WITH a params table. This script is the smallest thing that closes that: same
# contract as asserts.sh — offending rows, empty output is a pass, `-- requires:`
# skips, params from meta-r2.json overlaid with a variant — over DuckDB and
# parquet instead of sqlite and CSV.
#
# It also runs the RAW-record assertions here rather than under asserts.sh, on
# purpose. The CSV export decodes enum fields to names and renders 64-bit tags
# as hex strings while parquet leaves both as integers, so an assertion that
# does arithmetic on `tag` matches nothing at all on the CSV side and passes
# vacuously. Every tag-decoding file in asserts-r2/ carries its own canary for
# that, but the right fix is to run them where the columns are numbers.
set -u
PQ="${1:?usage: run-asserts-r2.sh <export.pq> <case-dir> [variant]}"
CASE_DIR="${2:?usage: run-asserts-r2.sh <export.pq> <case-dir> [variant]}"
VARIANT="${3:-}"
META="$CASE_DIR/meta-r2.json"
DUCK="${DUCKDB:-duckdb}"

[ -d "$PQ" ] || { echo "run-asserts-r2: no such export dir: $PQ" >&2; exit 2; }
command -v "$DUCK" >/dev/null || {
  echo "run-asserts-r2: no duckdb on PATH. The derived tables (transport_mix," >&2
  echo "  tag_flow, tag_recv_resolved, link_health) exist ONLY in the parquet" >&2
  echo "  export and there is no other documented way to query them with a" >&2
  echo "  params table. Falling back to asserts.sh would silently skip every" >&2
  echo "  assertion this round was written for, so this is a hard failure." >&2
  exit 2; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# ------------------------------------------------------------------ params ---
# Integer keys of "workload", overlaid with the selected variant. Same rule as
# asserts.sh: non-integers are skipped rather than coerced.
python3 - "$META" "$VARIANT" > "$TMP/params.csv" <<'PY' || exit 2
import json, sys
m = json.load(open(sys.argv[1]))
w = dict(m.get("workload", {}))
if sys.argv[2]:
    v = m.get("variants", {}).get(sys.argv[2])
    if v is None:
        sys.exit("no such variant %r" % sys.argv[2])
    w.update(v)
w = {k: v for k, v in w.items() if isinstance(v, int) and not isinstance(v, bool)}
if not w:
    sys.exit("workload block has no integer parameters")
print(",".join(w));  print(",".join(str(v) for v in w.values()))
PY

PARAM_COLS="$(head -1 "$TMP/params.csv" | tr ',' ' ')"
have_param() { for c in $PARAM_COLS; do [ "$c" = "$1" ] && return 0; done; return 1; }

# ------------------------------------------------------------------- views ---
# One view per parquet file, named by its basename — the same names the record
# types and derived tables carry everywhere else.
{
  echo "CREATE VIEW params AS SELECT * FROM read_csv_auto('$TMP/params.csv');"
  for f in "$PQ"/*.parquet; do
    [ -e "$f" ] || continue
    n="$(basename "$f" .parquet)"
    echo "CREATE VIEW \"$n\" AS SELECT * FROM read_parquet('$f');"
  done
} > "$TMP/views.sql"

TABLES=""
for f in "$PQ"/*.parquet; do [ -e "$f" ] && TABLES="$TABLES $(basename "$f" .parquet)"; done
have_table() { for t in $TABLES; do [ "$t" = "$1" ] && return 0; done; return 1; }

echo "  -- tables in $(basename "$PQ"):$TABLES"

# ------------------------------------------------------------ expect_tables --
tfail=0
echo "  -- expect_tables"
for t in $(python3 -c 'import json,sys;print(" ".join(json.load(open(sys.argv[1])).get("expect_tables",[])))' "$META"); do
  if ! have_table "$t"; then
    echo "  [XX ] $t — absent"; tfail=1
  else
    n="$("$DUCK" -noheader -list :memory: \
         "$(cat "$TMP/views.sql") SELECT count(*) FROM \"$t\";" 2>/dev/null)"
    if [ "${n:-0}" = 0 ]; then echo "  [XX ] $t — present but empty"; tfail=1
    else echo "  [ok ] $t ($n rows)"; fi
  fi
done

# -------------------------------------------------------------- assertions ---
afail=0; an=0; askip=0
echo "  -- assertions (offending rows; empty = pass)${VARIANT:+  [variant: $VARIANT]}"
for q in "$CASE_DIR"/asserts-r2/*.sql; do
  [ -e "$q" ] || continue
  name="$(basename "$q" .sql)"

  skip=""
  for req in $(sed -n 's/^-- requires:[[:space:]]*//p' "$q"); do
    case "$req" in
      params.*) have_param "${req#params.}" || skip="no param ${req#params.}" ;;
      *)        have_table "$req"           || skip="no table $req" ;;
    esac
    [ -n "$skip" ] && break
  done
  if [ -n "$skip" ]; then echo "  [skip] $name — $skip"; askip=$((askip+1)); continue; fi

  an=$((an+1))
  out="$("$DUCK" -noheader -list :memory: \
        "$(cat "$TMP/views.sql") $(cat "$q")" 2>&1)"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "  [ERR ] $name — query failed"; echo "$out" | sed 's/^/         /'
    afail=$((afail+1))
  elif [ -n "$out" ]; then
    echo "  [XX  ] $name"; echo "$out" | head -25 | sed 's/^/         /'
    afail=$((afail+1))
  else
    echo "  [ok  ] $name"
  fi
done

echo "  -- $an assertions, $afail failed, $askip skipped, expect_tables $([ $tfail = 0 ] && echo ok || echo FAILED)"
[ $afail = 0 ] && [ $tfail = 0 ]
