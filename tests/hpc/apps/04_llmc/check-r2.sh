#!/bin/bash
# Run every asserts/*_r2.sql over one export. Empty result = pass.
#
#   bash check-r2.sh /DATA/missa18/comshu-apps-r2/z0.pq
#
# The export must be a `comshu export --parquet` directory that `comshu link` has
# been run over — that is the only kind that has the derived tables at all
# (`comshu link` on a --csv export refuses: "no raw .parquet tables").
#
# This does the same job as tests/hpc/suite/sql_check.sh: it materialises the
# one-row `params` table from meta-r2.json's `workload` block and runs each
# assertion against it. It is separate only because sql_check.sh expects
# meta.json, and round 1's meta.json is a different topology that must not be
# edited.
set -u
EXPORT="${1:?usage: check-r2.sh <parquet-export-dir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD=${BUILD:-/DATA/missa18/build-llmc-r2}
export LD_LIBRARY_PATH="$BUILD/libcomshu:${LD_LIBRARY_PATH:-}"
COMSHU="$BUILD/comshu/comshu"

PARAMS=$(python3 -c '
import json, sys
w = json.load(open(sys.argv[1]))["workload"]
print(", ".join("%d AS %s" % (v, k) for k, v in w.items()))' "$HERE/meta-r2.json")

fail=0
have_table() {   # the suite's `-- requires:`, for tables only
  local t="$1"
  [ -n "$("$COMSHU" sql "$EXPORT" \
      "SELECT view_name FROM duckdb_views() WHERE view_name = '$t'" 2>/dev/null | sed 1d)" ]
}

for f in "$HERE"/asserts/*_r2.sql; do
  name=$(basename "$f" .sql)
  skip=
  for t in $(sed -n 's/^-- requires: //p' "$f" | tr ' ' '\n' | grep -v '^params\.'); do
    have_table "$t" || skip="$t"
  done
  if [ -n "$skip" ]; then echo "[skip]  $name — no $skip in this export"; continue; fi
  # Strip the leading comment block: `comshu sql` rejects a statement whose first
  # token is a comment (apps/README.md, "Known reader wart").
  # The trailing `;` goes too: `comshu sql` wraps the statement in
  # `COPY (<query>) TO '<tmp>.csv'`, so a semicolon inside the parentheses is a
  # parser error. Every assertion in tests/hpc/cases/ ends with one, so none of
  # them can be run through `comshu sql` as written — recorded in QUESTIONS.md.
  body=$(sed '/^--/d' "$f" | sed 's/;[[:space:]]*$//')
  case "$body" in
    WITH*) q="WITH params AS (SELECT $PARAMS), ${body#WITH }" ;;
    *)     q="WITH params AS (SELECT $PARAMS) $body" ;;
  esac
  out=$("$COMSHU" sql "$EXPORT" "$q" 2>&1)
  # A passing assertion returns the header row and nothing else.
  rows=$(printf '%s\n' "$out" | sed '1d' | grep -c . || true)
  if printf '%s' "$out" | grep -q '^comshu: query failed'; then
    echo "[ERROR] $name"; printf '%s\n' "$out" | head -5; fail=1
  elif [ "$rows" -eq 0 ]; then
    echo "[ok]    $name"
  else
    echo "[FAIL]  $name — $rows offending row(s)"; printf '%s\n' "$out" | head -8; fail=1
  fi
done
exit $fail
