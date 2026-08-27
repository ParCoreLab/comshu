#!/usr/bin/env bash
# check.sh — run this case's assertions over the two (three) CSV exports.
#
# WHY THIS EXISTS AND sql_check.sh DOES NOT SUFFICE. Two reasons, both about the
# oracle rather than about convenience:
#
#  1. sql_check.sh materialises `params` from a FIXED allowlist (PARAM_COLS:
#     ranks nodes gpus iters msg_bytes elems elem_bytes app_buffer_bytes
#     regions_per_rank umem_binding gpudirect rx_depth colls_per_iter
#     p2p_per_iter groups_per_iter). This case's oracle is `npool` — the flag
#     that partitions MPI_COMM_WORLD — and there is no slot for it. Spelling
#     `npool` as `groups_per_iter` would make the assertion unreadable and the
#     rule it obeys unverifiable, so the params table is built here instead,
#     from the same meta.json block, with no allowlist.
#
#  2. The headline claim compares TWO captures. sql_check.sh takes one export.
#     `asserts/knob_changes_the_matrix.sql` is marked `-- requires: two-captures`
#     and is run against a database holding both, with a `cfg` column.
#
# Everything else follows the suite: an assertion returns the OFFENDING ROWS and
# empty is a pass, and no assertion may name a rank, device or QP number the run
# happened to produce.
#
#   check.sh <npool1.csv> <npool2.csv> <rcverbs.csv> <case-dir>
set -uo pipefail

A_CSV="${1:?usage: check.sh <npool1.csv> <npool2.csv> <rcverbs.csv> <case-dir>}"
B_CSV="${2:?}"
V_CSV="${3:?}"
CASE_DIR="${4:?}"
DB_DIR="$(mktemp -d "${TMPDIR:-/tmp}/qe-check-XXXXXX")"
trap 'rm -rf "$DB_DIR"' EXIT

command -v sqlite3 >/dev/null || { echo "!! no sqlite3"; exit 2; }

# params, from meta.json's workload blocks. `python3` rather than jq: MN5 has
# python3 everywhere and jq nowhere reliable.
mk_params() {   # mk_params <block-name>
  python3 - "$CASE_DIR/meta.json" "$1" <<'PY'
import json, sys
w = json.load(open(sys.argv[1]))[sys.argv[2]]
cols = sorted(k for k, v in w.items() if isinstance(v, int))
print("CREATE TABLE params (%s);" % ", ".join("%s INTEGER" % c for c in cols))
print("INSERT INTO params VALUES (%s);" % ", ".join(str(w[c]) for c in cols))
PY
}

# THE DERIVED TABLES ARE NOT CSVs, and this script could not see them.
#
# `pcomm export --csv` writes one file per RAW record stream. comm_edges,
# link_health, wr_flow and the rest are DERIVED — built by `pcomm link` into the
# parquet directory beside the CSVs — so there is no comm_edges.csv to import.
# This loader imported `<csv>/*.csv` and nothing else, so every assertion over a
# derived table reported "comm_edges is EMPTY — unanswerable" against a table
# that existed and had rows. Measured on job 45103790: three assertions skipped
# that way while qe-npool2's comm_edges held six edges.
#
# So each derived table is materialised out of the parquet first, into the temp
# db dir rather than into the capture — a capture is an artefact and this script
# has no business writing into one. `pcomm sql` emits CSV on stdout, which is
# already the format the loader below wants.
#
# Best effort by design: a build without DuckDB has no `sql` verb and no
# parquet, and this must still run there. A table that cannot be materialised is
# simply absent, which the `-- requires:` machinery already reports honestly as
# a skip. The bug was never that the skip existed; it was that it was WRONG.
PCOMM="${PCOMM:-$HOME/proj/pcomm/build/pcomm/pcomm}"
DERIVED_TABLES="comm_edges link_health endpoint rank_identity wr_flow timeline_span object_access"

materialise_derived() {   # materialise_derived <csvdir> <outdir>
  local csv="$1" out="$2" pq="${1%.csv}.pq" t
  [ -d "$pq" ] || return 0
  [ -x "$PCOMM" ] || return 0
  mkdir -p "$out"
  for t in $DERIVED_TABLES; do
    "$PCOMM" sql "$pq" "SELECT * FROM $t" > "$out/$t.csv" 2>/dev/null || rm -f "$out/$t.csv"
    # An empty result still writes a header row; a failed one leaves nothing.
    [ -s "$out/$t.csv" ] || rm -f "$out/$t.csv"
  done
}

# Load one CSV export into a fresh sqlite db. Same convention as sql_check.sh:
# one table per <name>.csv, columns from the header row, everything TEXT — so
# every numeric comparison below CASTs, deliberately and visibly.
load() {        # load <db> <csvdir> <params-block>
  local db="$1" csv="$2" blk="$3"
  local der="$DB_DIR/derived-$(basename "$db" .db)"
  rm -f "$db"
  materialise_derived "$csv" "$der"
  { echo ".bail on"; echo ".mode csv"
    for f in "$csv"/*.csv "$der"/*.csv; do [ -e "$f" ] || continue
      echo ".import '$f' $(basename "$f" .csv)"; done
  } | sqlite3 "$db" 2>/dev/null
  mk_params "$blk" | sqlite3 "$db"
}

echo "=== loading captures ==="
load "$DB_DIR/a.db" "$A_CSV" workload
load "$DB_DIR/b.db" "$B_CSV" workload_b
[ -d "$V_CSV" ] && load "$DB_DIR/v.db" "$V_CSV" workload

# The combined database for the differential. Both captures' rows in one table
# set, tagged by `cfg` — 'a' is the npool=1 capture, 'b' is npool=2. Only the
# tables the differential needs are copied.
comb="$DB_DIR/both.db"; rm -f "$comb"
sqlite3 "$comb" <<SQL
ATTACH '$DB_DIR/a.db' AS a; ATTACH '$DB_DIR/b.db' AS b;
CREATE TABLE comm_edges   AS SELECT 'a' AS cfg, * FROM a.comm_edges;
INSERT INTO  comm_edges      SELECT 'b',        * FROM b.comm_edges;
CREATE TABLE ucx_tag_send AS SELECT 'a' AS cfg, * FROM a.ucx_tag_send;
INSERT INTO  ucx_tag_send    SELECT 'b',        * FROM b.ucx_tag_send;
SQL
mk_params workload | sqlite3 "$comb"

# ------------------------------------------------------------------ run -----
fail=0
# expect_fail_asserts from meta.json: {"<assert name>": "<why, with the blocking
# plan>"}. Same contract as the suite — reports [xfail] and does not fail the
# case, reports [XPASS] loudly if it starts passing. This is the mechanism that
# keeps "never edit an assertion to match what we emit" enforceable.
XFAIL="$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1])).get("expect_fail_asserts",{})))' "$CASE_DIR/meta.json" 2>/dev/null)"
is_xfail() { case " $XFAIL " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

run_one() {     # run_one <db> <label> <sqlfile>
  local db="$1" label="$2" f="$3" name; name="$(basename "$f" .sql)"
  # `-- requires: <table> ...` — skip when the capture has no such table, so an
  # assertion can be honest about a missing provider instead of erroring.
  # `-- requires: <table> ... params.<col>` — the suite's convention. A missing
  # or EMPTY table means the assertion could not be evaluated, which is a
  # different statement from "it failed" and must not be reported as one: on the
  # default UCX transport ib_post_send and comm_edges are legitimately empty
  # (nothing pcomm can hook is on that path), and an assertion over them is
  # UNANSWERABLE, not false. See REPORT.md finding 3.
  local need; need="$(sed -n 's/^-- requires:[[:space:]]*//p' "$f" | head -1)"
  for t in $need; do
    case "$t" in
      two-captures) continue ;;
      params.*)   # a workload parameter, not a table: check the column exists
        sqlite3 "$db" "SELECT ${t#params.} FROM params LIMIT 1;" >/dev/null 2>&1 \
          || { printf '  [skip] %-38s %s (workload has no %s)\n' "$name" "$label" "$t"; return; }
        continue ;;
    esac
    sqlite3 "$db" "SELECT 1 FROM \"$t\" LIMIT 1;" >/dev/null 2>&1 \
      || { printf '  [skip] %-38s %s (no table %s)\n' "$name" "$label" "$t"; return; }
    [ "$(sqlite3 "$db" "SELECT count(*) FROM \"$t\";" 2>/dev/null)" != "0" ] \
      || { printf '  [skip] %-38s %s (%s is EMPTY — unanswerable, not failed)\n' "$name" "$label" "$t"; return; }
  done
  local out; out="$(sqlite3 -header -column "$db" < "$f" 2>&1)"
  if [ -z "$out" ]; then
    if is_xfail "$name"; then printf '  [XPASS] %-37s %s  <- marked expect_fail and PASSED; re-read meta.json\n' "$name" "$label"
    else                      printf '  [PASS] %-38s %s\n' "$name" "$label"; fi
  elif is_xfail "$name"; then
    printf '  [xfail] %-37s %s\n' "$name" "$label"; echo "$out" | sed 's/^/         /'
  else
    printf '  [FAIL] %-38s %s\n' "$name" "$label"; echo "$out" | sed 's/^/         /'; fail=1
  fi
}

echo "=== assertions ==="
for f in "$CASE_DIR"/asserts/*.sql; do
  [ -e "$f" ] || continue
  case "$(sed -n 's/^-- requires:[[:space:]]*//p' "$f" | head -1)" in
    *two-captures*) run_one "$comb"      "[both]"    "$f" ;;
    *)              case "$(basename "$f")" in
                      npool1_*) run_one "$DB_DIR/a.db" "[npool=1]" "$f" ;;
                      npool2_*) run_one "$DB_DIR/b.db" "[npool=2]" "$f" ;;
                      *)        run_one "$DB_DIR/a.db" "[npool=1]" "$f"
                                run_one "$DB_DIR/b.db" "[npool=2]" "$f" ;;
                    esac ;;
  esac
done

# ------------------------------------------------------------- the tables ---
# Printed, not asserted. The report quotes these; a reader who does not trust a
# PASS can read the matrix that produced it.
echo
echo "=== rank-space comm matrix, pcomm's own comm_edges ==="
for cfg in a b v; do
  [ -f "$DB_DIR/$cfg.db" ] || continue
  echo "--- $cfg ($(case $cfg in a) echo 'npool=1, rc_verbs';; b) echo 'npool=2, rc_verbs';; v) echo 'npool=1, default TLS (rc_mlx5)';; esac)) ---"
  sqlite3 -header -column "$DB_DIR/$cfg.db" \
    "SELECT src_rank, dst_rank, sends, bytes FROM comm_edges ORDER BY CAST(src_rank AS INTEGER), CAST(dst_rank AS INTEGER);"
done

echo
echo "=== UCP endpoints that carried tagged sends, per rank (no peer rank: see finding 2) ==="
for cfg in a b v; do
  [ -f "$DB_DIR/$cfg.db" ] || continue
  echo "--- $cfg ---"
  sqlite3 -header -column "$DB_DIR/$cfg.db" <<'SQL'
SELECT CAST(rank AS INTEGER) AS rank, count(DISTINCT endpoint) AS endpoints, count(*) AS tag_sends
FROM ucx_tag_send GROUP BY 1 ORDER BY 1;
SQL
done

echo
echo "=== rank-space connection graph from the verbs layer (RC QPs per pair) ==="
for cfg in a b; do
  echo "--- npool=$([ $cfg = a ] && echo 1 || echo 2) ---"
  sqlite3 -header -column "$DB_DIR/$cfg.db" <<'SQL'
WITH owner AS (SELECT DISTINCT CAST(src_lid AS INTEGER) lid, CAST(qp_num AS INTEGER) qp,
                      CAST(rank AS INTEGER) rank FROM ib_modify_qp WHERE CAST(src_lid AS INTEGER) <> 0)
SELECT CAST(m.rank AS INTEGER) AS src, o.rank AS dst, count(DISTINCT m.qp_num) AS qps
FROM ib_modify_qp m
JOIN owner o ON o.lid = CAST(m.dest_lid AS INTEGER) AND o.qp = CAST(m.dest_qp_num AS INTEGER)
WHERE CAST(m.dest_lid AS INTEGER) <> 0
GROUP BY 1, 2 ORDER BY 1, 2;
SQL
done

echo
echo "=== stream row counts, per capture ==="
for cfg in a b v; do
  [ -f "$DB_DIR/$cfg.db" ] || continue
  echo "--- $cfg ---"
  sqlite3 "$DB_DIR/$cfg.db" \
    "SELECT name FROM sqlite_master WHERE type='table' AND name!='params' ORDER BY name;" \
  | while read -r t; do printf '  %-24s %s\n' "$t" "$(sqlite3 "$DB_DIR/$cfg.db" "SELECT count(*) FROM \"$t\";")"; done
done

echo
[ $fail -eq 0 ] && echo "RESULT: all assertions passed" || echo "RESULT: assertion failures above"
exit $fail
