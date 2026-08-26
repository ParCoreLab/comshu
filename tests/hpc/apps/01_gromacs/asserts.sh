#!/bin/bash
# asserts.sh — the assertion half of suite/sql_check.sh, with a `params` table
# this case can actually state its oracle in.
#
#   asserts.sh <csv-dir> <case-dir> [variant]
#
# WHY THIS EXISTS AND IS NOT A FORK FOR ITS OWN SAKE. sql_check.sh builds
# `params` from a HARD-CODED column list:
#
#   PARAM_COLS="ranks nodes gpus iters msg_bytes elems elem_bytes
#               app_buffer_bytes regions_per_rank umem_binding gpudirect
#               rx_depth colls_per_iter p2p_per_iter groups_per_iter"
#
# and exits 2 on any workload key outside it ("unknown workload key(s)"). Those
# fifteen columns are the vocabulary of the microbenchmarks the suite was written
# beside — an iteration count and a message size. This case's oracle is npp,
# npme, nsteps, nstlist and natoms, and not one of them can be written down. The
# assertions would have had to hide their oracle inside the SQL as bare literals,
# which is precisely the "golden wearing a costume" the suite forbids.
#
# So: same contract, same semantics, columns from meta.json.
#   - an assertion returns the OFFENDING ROWS; empty output is a pass
#   - `-- requires: <table> params.<col>` reports [skip] when the capture has no
#     such table or the workload no such parameter
#   - meta.json's expect_fail_asserts reports [xfail] / [XPASS]
#   - goldens are deliberately absent: this is a study, not a gate, and a golden
#     here could only ratify whatever the tool emitted the first time
#
# The `variant` argument selects a key from meta.json's "variants" object and
# overlays it on "workload". It exists so ONE assertion file can be the
# specification for BOTH arms of the direct-GPU-comm comparison: the arm is a
# parameter (`gpudirect`), not a different query.
set -u
CSV_DIR="${1:?usage: asserts.sh <csv-dir> <case-dir> [variant]}"
CASE_DIR="${2:?usage: asserts.sh <csv-dir> <case-dir> [variant]}"
VARIANT="${3:-}"
SQL_ENGINE="${SQL_ENGINE:-sqlite3}"
SUITE_DIR="$(cd "$CASE_DIR/../../suite" && pwd)"
meta="$CASE_DIR/meta.json"

[ -d "$CSV_DIR" ] || { echo "asserts.sh: no such export dir: $CSV_DIR" >&2; exit 2; }

DB="$(mktemp)"; trap 'rm -f "$DB"' EXIT
{
  echo ".mode csv"
  for f in "$CSV_DIR"/*.csv; do
    [ -s "$f" ] || continue
    echo ".import $f $(basename "$f" .csv)"
  done
} | "$SQL_ENGINE" "$DB" 2>/dev/null

sql() { "$SQL_ENGINE" "$DB" "$@"; }
have_table() { [ -n "$(sql "SELECT name FROM sqlite_master WHERE type IN ('table','view') AND name='$1';")" ]; }
row_count()  { sql "SELECT count(*) FROM \"$1\";" 2>/dev/null; }

# ---------------------------------------------------------------- params -----
# Every INTEGER-valued key of "workload", overlaid with the selected variant.
# Non-integer values (the variants' human-readable "env" note) are skipped
# rather than coerced, so meta.json can stay readable.
params_sql="$(python3 - "$meta" "$VARIANT" <<'PY'
import json, sys
meta, variant = sys.argv[1], sys.argv[2]
m = json.load(open(meta))
w = dict(m.get("workload", {}))
if variant:
    v = m.get("variants", {}).get(variant)
    if v is None:
        sys.exit("no such variant %r in %s" % (variant, meta))
    w.update(v)
w = {k: v for k, v in w.items() if isinstance(v, int) and not isinstance(v, bool)}
if not w:
    sys.exit("workload block has no integer parameters")
print("CREATE TABLE params (%s);" % ", ".join("%s INTEGER" % c for c in w))
print("INSERT INTO params VALUES (%s);" % ", ".join(str(v) for v in w.values()))
PY
2>&1)" || { echo "asserts.sh: $params_sql" >&2; exit 2; }
case "$params_sql" in
  "CREATE TABLE params"*) ;;
  *) echo "asserts.sh: bad workload/variants block: $params_sql" >&2; exit 2 ;;
esac
sql "$params_sql"
have_param() { sql "SELECT $1 FROM params;" >/dev/null 2>&1 && [ -n "$(sql "SELECT $1 FROM params WHERE $1 IS NOT NULL;")" ]; }

# ranks_seen, for the cross-cutting suite assertions that use it.
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

# --------------------------------------------------------- expect_tables -----
tfail=0
echo "  -- expect_tables"
for t in $(python3 -c 'import json,sys;print(" ".join(json.load(open(sys.argv[1])).get("expect_tables",[])))' "$meta"); do
  if ! have_table "$t"; then echo "  [XX ] $t — absent"; tfail=1
  elif [ "$(row_count "$t")" = 0 ]; then echo "  [XX ] $t — present but empty"; tfail=1
  else echo "  [ok ] $t ($(row_count "$t") rows)"; fi
done

# ------------------------------------------------------------ assertions -----
afail=0; an=0; askip=0; axfail=0; axpass=0
xfail_reason() {
  python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("expect_fail_asserts",{}).get(sys.argv[2],""))' "$meta" "$1"
}

echo "  -- assertions (offending rows; empty = pass)${VARIANT:+  [variant: $VARIANT]}"
for q in "$CASE_DIR"/asserts/*.sql "$SUITE_DIR"/asserts/*.sql; do
  [ -e "$q" ] || continue
  name="$(basename "$q" .sql)"

  skip=""
  for req in $(sed -n 's/^-- requires:[[:space:]]*//p' "$q"); do
    case "$req" in
      params.*) have_param "${req#params.}" || skip="no param ${req#params.}" ;;
      *)        { have_table "$req" && [ "$(row_count "$req")" != 0 ]; } || skip="no table $req" ;;
    esac
    [ -n "$skip" ] && break
  done
  if [ -n "$skip" ]; then echo "  [skip] $name — $skip"; askip=$((askip+1)); continue; fi

  an=$((an+1))
  out="$("$SQL_ENGINE" -csv -header "$DB" < "$q" 2>&1)"
  body="$(printf '%s\n' "$out" | tail -n +2 | sed '/^$/d')"
  why="$(xfail_reason "$name")"
  if [ -z "$body" ]; then
    if [ -n "$why" ]; then echo "  [XPASS] $name — marked expect_fail but PASSES; remove the marker"; axpass=$((axpass+1))
    else echo "  [ok ] $name"; fi
  else
    if [ -n "$why" ]; then
      echo "  [xfail] $name — ${why:0:150}"; axfail=$((axfail+1))
    else
      echo "  [XX ] $name — violated:"; afail=1
    fi
    printf '%s\n' "$out" | head -25 | sed 's/^/        /'
  fi
done

echo "  -- $an assertion(s), $askip skipped, $axfail xfail, $axpass XPASS; tables $( [ $tfail = 0 ] && echo ok || echo FAILED )"
[ "$afail" = 0 ] && [ "$tfail" = 0 ] && { echo "  RESULT: PASS"; exit 0; }
echo "  RESULT: FAIL"; exit 1
