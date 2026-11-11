#!/usr/bin/env bash
# check-r2.sh — run round 2's assertions over the two CSV exports.
#
# A derivative of this case's round-1 check.sh, not a replacement for it: round 1
# takes three captures and globs asserts/*.sql, which would now pick up both
# rounds' files and evaluate each against the other round's params. This one
# globs asserts/*-r2.sql, reads meta-r2.json, and takes two captures.
#
# Round 1's two reasons for not using suite/sql_check.sh both still hold:
#
#  1. sql_check.sh materialises `params` from a FIXED allowlist (PARAM_COLS) with
#     no slot for `npool`, and round 2 adds `nproc_bgrp`, `fft_nr3px`, `fft_ncpx`
#     to that problem. Spelling a communicator-partition knob as
#     `groups_per_iter` would make the assertions unreadable and the
#     "must reference params" rule unverifiable.
#  2. The headline claim is a comparison of two captures. sql_check.sh takes one.
#     Here the comparison is carried INSIDE the assertion instead: every -r2
#     assertion is evaluated against both arms with that arm's params, and
#     transport_mix_follows_the_pool_boundary-r2.sql makes the opposite claim in
#     each because it branches on `npool < nodes`. That is better than a joined
#     database — it keeps each claim a single-capture statement, so a reader can
#     re-run one arm on its own.
#
# Everything else follows the suite: an assertion returns the OFFENDING ROWS,
# empty is a pass, and no assertion may name a rank, device, endpoint or
# transport that the run happened to produce.
#
#   check-r2.sh <npool1.csv> <npool2.csv> <case-dir>
set -uo pipefail

A_CSV="${1:?usage: check-r2.sh <npool1.csv> <npool2.csv> <case-dir>}"
B_CSV="${2:?}"
CASE_DIR="${3:?}"
META="$CASE_DIR/meta-r2.json"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/qe-r2-XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

command -v sqlite3 >/dev/null || { echo "!! no sqlite3"; exit 2; }
[ -f "$META" ]              || { echo "!! no $META"; exit 2; }

# params, from meta-r2.json's workload blocks. python3 rather than jq: MN5 has
# python3 everywhere and jq nowhere reliable.
mk_params() {   # mk_params <block-name>
  python3 - "$META" "$1" <<'PY'
import json, sys
w = json.load(open(sys.argv[1]))[sys.argv[2]]
cols = sorted(k for k, v in w.items() if isinstance(v, int))
print("CREATE TABLE params (%s);" % ", ".join("%s INTEGER" % c for c in cols))
print("INSERT INTO params VALUES (%s);" % ", ".join(str(w[c]) for c in cols))
PY
}

# THE DERIVED TABLES ARE NOT CSVs. `comshu export --csv` writes one file per RAW
# record stream; transport_mix, tag_flow, link_health and the rest are built by
# `comshu link` into the parquet directory beside them. Round 1 learned this the
# expensive way (job 45103790: three assertions reported "EMPTY — unanswerable"
# against tables that existed and had rows), so each derived table is
# materialised out of the parquet with `comshu sql`, which already emits CSV.
#
# transport_mix and tag_flow are this round's headline tables and lead the list.
COMSHU="${COMSHU:-$HOME/proj/comshu/build/comshu/comshu}"
DERIVED_TABLES="transport_mix tag_flow tag_recv_resolved link_health rank_identity endpoint comm_edges object_access"

materialise_derived() {   # materialise_derived <csvdir> <outdir>
  local csv="$1" out="$2" pq="${1%.csv}.pq" t
  [ -d "$pq" ] || { echo "   (no $pq — derived layer absent; comshu_derive needs DuckDB)" >&2; return 0; }
  [ -x "$COMSHU" ] || { echo "   (no $COMSHU — cannot materialise derived tables)" >&2; return 0; }
  mkdir -p "$out"
  for t in $DERIVED_TABLES; do
    if "$COMSHU" sql "$pq" "SELECT * FROM $t" > "$out/$t.csv" 2>"$out/$t.err"; then
      [ -s "$out/$t.csv" ] || rm -f "$out/$t.csv"
    else
      echo "   (derived table $t unavailable: $(head -1 "$out/$t.err" 2>/dev/null))" >&2
      rm -f "$out/$t.csv"
    fi
  done
  rm -f "$out"/*.err
}

# One table per <name>.csv, columns from the header row, everything TEXT — the
# same convention as sql_check.sh, which is why every numeric comparison in the
# assertions CASTs, deliberately and visibly.
load() {        # load <db> <csvdir> <params-block>
  local db="$1" csv="$2" blk="$3"
  local der="$DB_DIR/derived-$(basename "$db" .db)"
  rm -f "$db"
  [ -d "$csv" ] || { echo "!! no CSV export at $csv"; return 1; }
  materialise_derived "$csv" "$der"
  { echo ".bail on"; echo ".mode csv"
    for f in "$csv"/*.csv; do [ -e "$f" ] || continue
      echo ".import '$f' $(basename "$f" .csv)"; done
  } | sqlite3 "$db" 2>/dev/null
  # A SEPARATE INVOCATION for the derived tables. `.bail on` stops at the first
  # failing .import, so appending these to the list above lets ONE malformed raw
  # CSV silently take every derived table with it — and the assertions over them
  # then read zeros with nothing saying why. Round 1's comment records the
  # measurement.
  #
  # The raw export and the derived layer both produce a `comm_edges` and they are
  # NOT the same table (four columns vs six). sqlite maps .import positionally
  # and only warns, so `transport` would land in `sends`. Drop before import.
  if [ -d "$der" ]; then
    local errs
    errs="$( { echo ".mode csv"
               for f in "$der"/*.csv; do [ -e "$f" ] || continue
                 echo "DROP TABLE IF EXISTS $(basename "$f" .csv);"
                 echo ".import '$f' $(basename "$f" .csv)"; done
             } | sqlite3 "$db" 2>&1 )"
    [ -n "$errs" ] && echo "!! derived-table import: $errs" >&2
  fi
  mk_params "$blk" | sqlite3 "$db"
}

echo "=== loading captures ==="
load "$DB_DIR/a.db" "$A_CSV" workload
load "$DB_DIR/b.db" "$B_CSV" workload_b

fail=0
XFAIL="$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1])).get("expect_fail_asserts",{})))' "$META" 2>/dev/null)"
is_xfail() { case " $XFAIL " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

run_one() {     # run_one <db> <label> <sqlfile>
  local db="$1" label="$2" f="$3" name; name="$(basename "$f" .sql)"
  # `-- requires:` must distinguish THREE states, not two: a table that is
  # missing, a table that exists but is EMPTY, and a params column that is
  # absent are all "cannot evaluate", and none of them is a failure. On the
  # default UCX transport ib_post_send is legitimately empty, and if the
  # ucx_deep guard refuses then uct_send is too — those assertions are
  # UNANSWERABLE, not false.
  local need; need="$(sed -n 's/^-- requires:[[:space:]]*//p' "$f" | head -1)"
  for t in $need; do
    case "$t" in
      params.*)
        sqlite3 "$db" "SELECT ${t#params.} FROM params LIMIT 1;" >/dev/null 2>&1 \
          || { printf '  [skip] %-46s %s (workload has no %s)\n' "$name" "$label" "$t"; return; }
        continue ;;
    esac
    sqlite3 "$db" "SELECT 1 FROM \"$t\" LIMIT 1;" >/dev/null 2>&1 \
      || { printf '  [skip] %-46s %s (no table %s)\n' "$name" "$label" "$t"; return; }
    [ "$(sqlite3 "$db" "SELECT count(*) FROM \"$t\";" 2>/dev/null)" != "0" ] \
      || { printf '  [skip] %-46s %s (%s is EMPTY — unanswerable, not failed)\n' "$name" "$label" "$t"; return; }
  done
  local out; out="$(sqlite3 -header -column "$db" < "$f" 2>&1)"
  if [ -z "$out" ]; then
    if is_xfail "$name"; then printf '  [XPASS] %-45s %s  <- marked expect_fail and PASSED; re-read meta-r2.json\n' "$name" "$label"
    else                      printf '  [PASS] %-46s %s\n' "$name" "$label"; fi
  elif is_xfail "$name"; then
    printf '  [xfail] %-45s %s\n' "$name" "$label"; echo "$out" | sed 's/^/         /'
  else
    printf '  [FAIL] %-46s %s\n' "$name" "$label"; echo "$out" | sed 's/^/         /'; fail=1
  fi
}

echo "=== assertions (round 2) ==="
for f in "$CASE_DIR"/asserts/*-r2.sql; do
  [ -e "$f" ] || continue
  run_one "$DB_DIR/a.db" "[npool=1]" "$f"
  run_one "$DB_DIR/b.db" "[npool=2]" "$f"
done

# ------------------------------------------------------------ the diff ------
# Printed, not asserted. The assertions above already carry the claim; this is
# the table a reader wants to see beside them, and it is the one artefact of
# this study that a human should look at before believing any of it.
echo
echo "=== the knob, side by side: UCT operations by transport ==="
for cfg in a b; do
  db="$DB_DIR/$cfg.db"
  echo "--- npool=$([ $cfg = a ] && echo 1 || echo 2)"
  sqlite3 -header -column "$db" \
    "SELECT tl_name,
            CASE WHEN tl_name LIKE '%mlx5%' OR tl_name LIKE '%verbs%'
                 THEN 'fabric' ELSE 'node-local' END AS kind,
            sum(CAST(ops AS INTEGER))              AS ops,
            sum(CAST(out_bytes AS INTEGER))        AS out_bytes,
            sum(CAST(advertised_bytes AS INTEGER)) AS advertised_bytes
     FROM transport_mix GROUP BY 1,2 ORDER BY 3 DESC;" 2>&1 | sed 's/^/    /'
  echo "    -- by op family (payload means a different thing per family; do not sum) --"
  sqlite3 -header -column "$db" \
    "SELECT op, sum(CAST(ops AS INTEGER)) AS ops FROM transport_mix
     GROUP BY 1 ORDER BY 2 DESC;" 2>&1 | sed 's/^/    /'
  echo "    -- tagged-send sizes, top 5 by volume --"
  sqlite3 -header -column "$db" \
    "SELECT CAST(count AS INTEGER) AS bytes, count(*) AS n FROM ucx_tag_send
     GROUP BY 1 ORDER BY n DESC LIMIT 5;" 2>&1 | sed 's/^/    /'
done

echo
echo "=== tables present, per arm ==="
for cfg in a b; do
  echo "--- npool=$([ $cfg = a ] && echo 1 || echo 2)"
  sqlite3 "$DB_DIR/$cfg.db" \
    "SELECT name FROM sqlite_master WHERE type='table' AND name!='params' ORDER BY name;" \
    2>/dev/null | tr '\n' ' ' | fold -w 100 -s | sed 's/^/    /'
  echo
done

exit $fail
