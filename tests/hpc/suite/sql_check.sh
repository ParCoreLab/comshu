#!/bin/bash
# sql_check.sh — run a case's SQL assertions against a pcomm-read CSV export.
#
#   sql_check.sh <csv-dir> <case-dir> [--update]
#
# The capture is loaded into an in-memory relational form (one table per
# <name>.csv, columns from the header row) and each checks/<name>.sql is run;
# its output is diffed against checks/<name>.golden. The SQL binds to column
# names — not the on-disk record format — so a format change touches pcomm-read,
# not the tests. Each query must be deterministic (aggregate away run-varying
# columns: ticks, qp_num/LID values, addresses, wr_id) and ORDER BY its output,
# so the golden is byte-repeatable. --update regenerates goldens from this run
# (use only when you've independently confirmed the numbers are correct).
#
# Engine is sqlite3 (present everywhere, zero setup); queries are kept portable
# ANSI SQL so DuckDB can be dropped in by changing SQL_ENGINE.
set -u
CSV_DIR="${1:?usage: sql_check.sh <csv-dir> <case-dir> [--update]}"
CASE_DIR="${2:?usage: sql_check.sh <csv-dir> <case-dir> [--update]}"
UPDATE="${3:-}"
SQL_ENGINE="${SQL_ENGINE:-sqlite3}"

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
checks_dir="$CASE_DIR/checks"
[ -d "$checks_dir" ] || { echo "$case_name: no checks/ dir" >&2; exit 2; }

fail=0; n=0
for q in "$checks_dir"/*.sql; do
  [ -e "$q" ] || { echo "$case_name: no .sql checks" >&2; exit 2; }
  name="$(basename "$q" .sql)"
  golden="$checks_dir/$name.golden"
  got="$("$SQL_ENGINE" -csv -header "$DB" < "$q" 2>&1)"
  n=$((n+1))
  if [ "$UPDATE" = "--update" ]; then
    printf '%s\n' "$got" > "$golden"; echo "  [updated] $name"; continue
  fi
  if [ ! -f "$golden" ]; then
    echo "  [MISS] $name — no golden (run with --update once verified)"; fail=1; continue
  fi
  if diff -q <(printf '%s\n' "$got") "$golden" >/dev/null; then
    echo "  [ok ] $name"
  else
    echo "  [XX ] $name — differs from golden:"
    diff <(printf '%s\n' "$got") "$golden" | sed 's/^/        /'
    fail=1
  fi
done

if [ "$UPDATE" = "--update" ]; then echo "$case_name: $n golden(s) written"; exit 0; fi
if [ "$fail" -eq 0 ]; then echo "=== $case_name : PASS ($n checks) ==="
else echo "=== $case_name : FAIL ==="; fi
# machine-readable verdict for the aggregator
printf '{"case":"%s","ok":%s,"checks":%d}\n' "$case_name" \
       "$([ $fail -eq 0 ] && echo true || echo false)" "$n" > "$CSV_DIR/result.json"
exit $fail
