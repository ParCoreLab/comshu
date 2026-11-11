#!/bin/bash
# run-asserts.sh — run asserts/*.sql against a linked Parquet export.
#
#   run-asserts.sh <export.pq> <case-dir> [variant]
#
# Empty result = pass, as everywhere else in this repo.
#
# WHY THIS EXISTS RATHER THAN suite/sql_check.sh. Two reasons:
#
#   * sql_check.sh builds `params` from a CLOSED fifteen-name column list and
#     rejects the WHOLE meta.json for any other key. This case's oracle is a
#     gradient element count, a group-boundary count and an information bound,
#     none of which are in that vocabulary.
#   * sql_check.sh loads a CSV export into sqlite3, and `comshu export --csv`
#     emits one CSV per RECORD TYPE. comm_edges, rank_identity and link_health
#     are built by `comshu link` and live only in the PARQUET export, so three of
#     the tables this study exists to test are unreachable that way.
#
# WHY `comshu sql` AND NOT A duckdb BINARY. 01_gromacs's round-2 runner shells
# out to `duckdb`, which is not on PATH on MN5 — `which duckdb` finds nothing on
# glogin1 — and hard-fails when it is missing. `comshu sql` is the documented
# query interface, it is DuckDB underneath on a COMSHU_DUCKDB=ON build, and round
# 1 of 02_lammps already drove every assertion through it successfully on this
# exact machine. Two documented warts are worked around here so that the .sql
# files stay ordinary SQL:
#   * a statement whose FIRST token is a comment is rejected (documented in
#     apps/README.md), so leading comment lines are stripped;
#   * the text is wrapped in `COPY (<query>) TO '<tmp>.csv'`, which makes a
#     TRAILING SEMICOLON a syntax error too (NOTES.md #4).
# `params` is materialised as a leading CTE, which is why a single statement is
# enough and no second runner is needed.
set -u
PQ="${1:?usage: run-asserts.sh <export.pq> <case-dir> [variant]}"
CASE_DIR="${2:?usage: run-asserts.sh <export.pq> <case-dir> [variant]}"
VARIANT="${3:-}"
META="$CASE_DIR/meta.json"
COMSHU="${COMSHU:?COMSHU must point at the comshu binary (source tests/hpc/suite/common.sh)}"

[ -d "$PQ" ] || { echo "run-asserts: no such export dir: $PQ" >&2; exit 2; }

# ------------------------------------------------------------------ params ---
# Integer keys of "workload", overlaid with the selected variant. Non-integers
# are skipped rather than coerced, same rule the other app runners use.
PARAMS="$(python3 - "$META" "$VARIANT" <<'PY'
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
print("WITH params AS (SELECT " + ", ".join("%d AS %s" % (v, k) for k, v in w.items()) + ") ")
PY
)" || exit 2

PARAM_COLS="$(python3 - "$META" "$VARIANT" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
w = dict(m.get("workload", {}))
if sys.argv[2]:
    w.update(m.get("variants", {}).get(sys.argv[2], {}))
print(" ".join(k for k, v in w.items() if isinstance(v, int) and not isinstance(v, bool)))
PY
)"
have_param() { for c in $PARAM_COLS; do [ "$c" = "$1" ] && return 0; done; return 1; }

# ------------------------------------------------------------------ tables ---
# An export is a directory of <name>.parquet files. Round 2 of 04_llmc recorded
# that `comshu export --parquet` writes a raw table only when it HAS rows, while
# `comshu link` writes its derived tables out even when empty — so presence is a
# filesystem question, not a query.
TABLES=""
for f in "$PQ"/*.parquet; do [ -e "$f" ] && TABLES="$TABLES $(basename "$f" .parquet)"; done
have_table() { for t in $TABLES; do [ "$t" = "$1" ] && return 0; done; return 1; }
echo "  -- tables in $(basename "$PQ"):$TABLES"

# ------------------------------------------------------------ expect_tables --
tfail=0
echo "  -- expect_tables"
for t in $(python3 -c 'import json,sys;print(" ".join(json.load(open(sys.argv[1])).get("expect_tables",[])))' "$META"); do
  if ! have_table "$t"; then
    echo "  [XX  ] $t — absent"; tfail=1
  else
    n="$("$COMSHU" sql "$PQ" "SELECT count(*) AS n FROM \"$t\"" 2>/dev/null | tail -1)"
    case "${n:-0}" in 0|"") echo "  [XX  ] $t — present but empty"; tfail=1 ;;
                      *)    echo "  [ok  ] $t ($n rows)" ;; esac
  fi
done

# -------------------------------------------------------------- assertions ---
XFAIL="$(python3 -c 'import json,sys;print(" ".join(json.load(open(sys.argv[1])).get("expect_fail_asserts",{})))' "$META")"
afail=0; an=0; askip=0
echo "  -- assertions (offending rows; empty = pass)${VARIANT:+  [variant: $VARIANT]}"
for q in "$CASE_DIR"/asserts/*.sql; do
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
  body="$(sed '/^[[:space:]]*--/d; /^[[:space:]]*$/d' "$q" | sed '$ s/;[[:space:]]*$//')"
  # `params` is a leading CTE, so an assertion that opens with its OWN `WITH`
  # would produce `WITH params AS (...) WITH universe AS (...)`, which is a
  # Parser Error and cost job 45329318 five of its nine assertions. Splice the
  # two CTE lists instead: the file's leading WITH becomes a comma. Anything
  # else -- a bare SELECT -- is appended unchanged.
  case "$body" in
    WITH\ *|WITH$'\n'*|WITH$'\t'*) body=", ${body#WITH}" ;;
  esac
  out="$("$COMSHU" sql "$PQ" "$PARAMS$body" 2>&1)"
  rows="$(printf '%s\n' "$out" | tail -n +2 | grep -c . || true)"
  case " $XFAIL " in *" $name "*) mark="xfail" ;; *) mark="" ;; esac

  if printf '%s' "$out" | grep -qiE "query failed|Parser Error|Binder Error|Catalog Error|Conversion Error"; then
    echo "  [ERR ] $name"; printf '%s\n' "$out" | head -6 | sed 's/^/         /'; afail=$((afail+1))
  elif [ "$rows" -eq 0 ]; then
    if [ -n "$mark" ]; then echo "  [XPASS] $name — passing while marked expect_fail; re-check the marker"
    else echo "  [ok  ] $name"; fi
  else
    if [ -n "$mark" ]; then echo "  [xfail] $name — $rows offending row(s), known:"
    else echo "  [XX  ] $name — $rows offending row(s):"; afail=$((afail+1)); fi
    printf '%s\n' "$out" | head -14 | sed 's/^/         /'
  fi
done

echo "  -- $an assertions, $afail failed, $askip skipped, expect_tables $([ $tfail = 0 ] && echo ok || echo FAILED)"
[ $afail = 0 ] && [ $tfail = 0 ]
