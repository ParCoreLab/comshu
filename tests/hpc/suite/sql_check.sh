#!/bin/bash
# sql_check.sh — run a case's SQL checks against a `pcomm export --csv` export.
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
#
# --- ASSERTIONS PREFER PARQUET, GOLDENS NEVER DO ----------------------------
#
# The CSV export cannot carry the derived layer. It re-derives ONE derived table,
# comm_edges, in C++ (CsvAgg, pcomm-read/main.cpp) — a second implementation of
# the most-asserted table in the suite, kept byte-identical to link_sql.hpp's
# version by inspection. Everything else the UI computes (object_access,
# link_health, rank_identity, endpoint, timeline_span, wr_flow) is invisible to
# every assertion ever written.
#
# That is not a gap in coverage, it is a gap in *what can be covered*, and it
# cost a real bug: object_access.owner_rank named the writing rank rather than
# the owning one for the p2p branch's whole life, reporting the hubspoke case's
# one shared buffer as three phantom objects. Case 13's own assertion could not
# see it — it counts writers straight off gpu_access — so it passed throughout.
#
# So when a linked Parquet export is available, ASSERTIONS run against it via
# `pcomm sql`, which is the same DuckDB and the same link_sql.hpp the UI serves.
# One derived layer, asserted directly.
#
# GOLDENS STAY ON SQLITE/CSV, deliberately. A golden is a characterization test
# diffed against a stored file, so changing its engine changes every stored
# file's formatting and would force a mass regeneration — and a carelessly
# regenerated golden enshrines whatever is there today (see 04_nvshmem's
# peer_fanout above). Assertions have nothing to regenerate, which is exactly
# why they are the half that can move.
#
# Falls back silently to CSV+sqlite when there is no Parquet export or no
# DuckDB-capable pcomm: MN5 builds with PCOMM_DUCKDB=OFF by default because
# tools/get_duckdb.sh needs network and MN5 has none. The fallback must stay
# exact — a suite that only works on the workstation is not a suite.
set -u
CSV_DIR="${1:?usage: sql_check.sh <csv-dir> <case-dir> [--update]}"
CASE_DIR="${2:?usage: sql_check.sh <csv-dir> <case-dir> [--update]}"
UPDATE="${3:-}"
SQL_ENGINE="${SQL_ENGINE:-sqlite3}"
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The linked Parquet export, if one was made. Convention: beside the CSV dir,
# with .pq where .csv is — so a case that adds `--parquet` gets this for free and
# one that does not is unaffected.
PQ_DIR="${PCOMM_PQ_DIR:-${CSV_DIR%.csv}.pq}"
# A pcomm that actually has DuckDB. $PCOMM is set by common.sh; the verb list is
# the only honest test, because a build without DuckDB still ADVERTISES `sql` and
# refuses at runtime.
PQ_BIN="${PCOMM_PQ_BIN:-${PCOMM:-}}"
PQ_MODE=0
if [ -n "$PQ_BIN" ] && [ -x "$PQ_BIN" ] && [ -d "$PQ_DIR" ] &&
   ls "$PQ_DIR"/*.parquet >/dev/null 2>&1 &&
   "$PQ_BIN" sql "$PQ_DIR" "SELECT 1" >/dev/null 2>&1; then
  PQ_MODE=1
fi

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
# A case needs SOMETHING to check, but goldens are not that something. Requiring
# checks/ rejected the first assertions-only case outright — it exited 2 before
# running a single assertion and wrote no result.json, so the board came back
# EMPTY rather than red, which is worse than either verdict. Cases are supposed
# to be moving away from goldens; the harness has to allow arriving.
[ -d "$checks_dir" ] || [ -d "$CASE_DIR/asserts" ] || {
  echo "$case_name: neither checks/ nor asserts/ — a case must check something" >&2
  exit 2; }

sql() { "$SQL_ENGINE" "$DB" "$@"; }
have_table() { [ -n "$(sql "SELECT name FROM sqlite_master WHERE type IN ('table','view') AND name='$1';")" ]; }
row_count()  { sql "SELECT count(*) FROM \"$1\";" 2>/dev/null; }

# ---------------------------------------------------------------- params -----
# The workload block is a flat object of integers (or null). The column list is
# fixed and shared by every case so an assertion referencing a param a case does
# not define gets NULL — which `-- requires:` turns into a visible [skip] rather
# than a query that quietly matches nothing and reads as a pass.
# umem_binding is the transport configuration as an oracle input, not an
# observation: IBGDA hands buffers to the NIC either by address (peermem path,
# NVSHMEM_IB_DISABLE_DMABUF=1) or by dmabuf fd, and the two produce *different
# correct captures* — by address the umem carries a real pointer, by dmabuf it
# carries an offset of 0 and a descriptor. A capture that does not say which was
# configured cannot be checked against either, which is exactly how hours went
# into comparing two runs that were never the same experiment. 0 = by_addr,
# 1 = by_dmabuf.
# app_buffer_bytes is what the APPLICATION allocates in one buffer — for putget,
# elems * sizeof(int). A separate param rather than derived from
# elems * elem_bytes, because deriving it would apply to cases 02, 03 and 05
# too, and those are NCCL and plain-CUDA workloads that never allocate a
# symmetric buffer: `elems` there is a collective's element count. A case opts
# in by declaring it, which is what makes app_buffers_recorded a check rather
# than noise on a third of the board.
# rx_depth is the receive-queue depth a workload pre-posts. It is a genuine
# oracle and not an observation: ibv_rc_pingpong takes it as -r (default 500)
# and posts exactly that many receives up front, so the expected ib_post_recv
# count is known from the command line before the code runs.
PARAM_COLS="ranks nodes gpus iters msg_bytes elems elem_bytes app_buffer_bytes regions_per_rank umem_binding gpudirect rx_depth"
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
# Belt as well as braces. The guard above returns non-zero and its message was
# still lost once: every assertion then reported "no such table: params", which
# names the symptom rather than the cause and reads as five broken assertions
# instead of one unrecognised key. Check the value itself.
case "$params_sql" in
  "CREATE TABLE params"*) ;;
  *) echo "$case_name: bad workload block in meta.json: $params_sql" >&2; exit 2 ;;
esac
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
  # No goldens is a legitimate state now, not an error: a case whose every
  # invariant is allocation-dependent has nothing honest to characterize. Bail
  # out only if there are no assertions either — that check is above.
  [ -e "$q" ] || { echo "     (none — this case asserts instead)"; break; }
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

# --------------------------------------------------- the Parquet assertion path
# `pcomm sql` wraps its argument in COPY (...) TO, so it takes exactly ONE
# statement and no DDL. `params` and `ranks_seen` are synthetic tables that exist
# only in the sqlite DB, so on this path they have to arrive as CTEs spliced onto
# the front of the assertion. Splicing is done in python rather than sed because
# the three cases (`WITH RECURSIVE ...`, `WITH ...`, bare SELECT) differ in where
# the comma goes, and getting that wrong produces a parse error that reads like a
# broken assertion.
pq_sql() { "$PQ_BIN" sql "$PQ_DIR" "$1" 2>&1; }

pq_has_table() { [ -e "$PQ_DIR/$1.parquet" ]; }
pq_row_count() {
  pq_has_table "$1" || { echo 0; return; }
  pq_sql "SELECT count(*) FROM \"$1\"" | tail -1
}

# Does this assertion need a table only the Parquet export has? If the CSV set
# already carries every table it names, the sqlite run above already checked it
# and re-checking proves nothing.
pq_only() {
  need="$(sed -n 's/^-- requires:[[:space:]]*//p' "$1" | tr '\n' ' ')"
  for r in $need; do
    case "$r" in
      params.*) ;;
      *) if ! have_table "$r" && pq_has_table "$r"; then return 0; fi ;;
    esac
  done
  return 1
}

pq_prelude_values="$(meta_py <<'PY'
import json, sys
meta, cols = sys.argv[1], sys.argv[2].split()
w = json.load(open(meta)).get("workload", {})
print(", ".join(("NULL" if w.get(c) is None else str(int(w[c]))) + " AS " + c for c in cols))
PY
)"

# ranks_seen, rebuilt over the Parquet tables that carry a rank column. Same
# definition as the sqlite one — "a rank appears in whichever tables the capture
# happens to have" — over a different table set.
pq_rank_union=""
if [ "$PQ_MODE" -eq 1 ]; then
  for f in "$PQ_DIR"/*.parquet; do
    t="$(basename "$f" .parquet)"
    pq_sql "SELECT rank FROM \"$t\" LIMIT 0" >/dev/null 2>&1 || continue
    if [ -z "$pq_rank_union" ]; then pq_rank_union="SELECT DISTINCT rank FROM \"$t\""
    else pq_rank_union="$pq_rank_union UNION SELECT DISTINCT rank FROM \"$t\""; fi
  done
fi
[ -n "$pq_rank_union" ] || pq_rank_union="SELECT NULL AS rank WHERE 1=0"

run_assert_pq() {
  q="$1"; name="$(basename "$q" .sql)"
  need="$(sed -n 's/^-- requires:[[:space:]]*//p' "$q" | tr '\n' ' ')"
  for r in $need; do
    case "$r" in
      params.*) col="${r#params.}"
        if [ "$(pq_sql "WITH params AS (SELECT $pq_prelude_values) SELECT count(*) FROM params WHERE $col IS NOT NULL" | tail -1)" = "0" ]; then
          echo "  [skip] $name (derived) — workload has no $col"; askip=$((askip+1)); return; fi ;;
      *) if ! pq_has_table "$r" || [ "$(pq_row_count "$r")" = "0" ]; then
           echo "  [skip] $name (derived) — no $r rows in this capture"; askip=$((askip+1)); return; fi ;;
    esac
  done
  an=$((an+1))
  spliced="$(PQ_VALUES="$pq_prelude_values" PQ_RANKS="$pq_rank_union" python3 - "$q" <<'PY'
import os, re, sys
body = open(sys.argv[1]).read()
# Strip leading comments and blank lines so the WITH/SELECT is the first token.
body = re.sub(r'(?m)^\s*--.*$', '', body).strip().rstrip(';')
ctes = 'params AS (SELECT %s), ranks_seen AS (%s)' % (os.environ['PQ_VALUES'], os.environ['PQ_RANKS'])
m = re.match(r'(?is)^with\s+recursive\s+', body)
if m:
    print('WITH RECURSIVE %s, %s' % (ctes, body[m.end():]))
else:
    m = re.match(r'(?is)^with\s+', body)
    if m:
        print('WITH %s, %s' % (ctes, body[m.end():]))
    else:
        print('WITH %s %s' % (ctes, body))
PY
)"
  out="$(pq_sql "$spliced")"
  # `pcomm sql` prints a header row for a successful empty result; the header
  # alone therefore means PASS. Anything with a second line is a violation, and
  # its own error text (no header) is a broken assertion, not a passing one.
  case "$out" in
    pcomm:*|*"Error"*) echo "  [XX ] $name (derived) — query failed:"
                       printf '%s\n' "$out" | head -6 | sed 's/^/        /'; afail=1; return ;;
  esac
  rows="$(printf '%s\n' "$out" | tail -n +2)"
  why="$(xfail_reason "$name")"
  if [ -z "$rows" ]; then
    if [ -n "$why" ]; then
      echo "  [XPASS] $name (derived) — marked expect_fail but passes"; axpass=$((axpass+1))
    else echo "  [ok ] $name (derived)"; fi
  elif [ -n "$why" ]; then
    echo "  [xfail] $name (derived) — known: $why"
    printf '%s\n' "$rows" | head -12 | sed 's/^/        /'; axfail=$((axfail+1))
  else
    echo "  [XX ] $name (derived) — violated by:"
    printf '%s\n' "$rows" | head -25 | sed 's/^/        /'; afail=1
  fi
}

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

# --- the same assertions again, against the derived layer --------------------
#
# Only the ones that a Parquet export can answer AND the CSV export cannot: an
# assertion naming a derived-only table was [skip]ped above because the table is
# simply not in the CSV set. Re-running the whole suite of them here would double
# every verdict; re-running only these turns "cannot be checked" into "checked".
#
# Reported with a (derived) tag so a failure is unambiguous about which engine
# saw it. The two engines disagreeing about the same assertion is itself a
# finding worth reading — it means CsvAgg's comm_edges and link_sql.hpp's have
# drifted, which is precisely the fork this path exists to expose.
if [ "$PQ_MODE" -eq 1 ]; then
  echo "  -- assertions against the derived layer ($(basename "$PQ_DIR"), via pcomm sql)"
  for q in "$SUITE_DIR"/asserts/*.sql "$CASE_DIR"/asserts/*.sql; do
    [ -e "$q" ] || continue
    pq_only "$q" || continue
    run_assert_pq "$q"
  done
fi

# ------------------------------------------------------------------ verdict --
fail=$(( gfail || afail ))
n=$((gn + an))
if [ "$fail" -eq 0 ]; then echo "=== $case_name : PASS ($gn goldens, $an assertions) ==="
else echo "=== $case_name : FAIL (goldens $([ $gfail -eq 0 ] && echo ok || echo FAILED), assertions $([ $afail -eq 0 ] && echo ok || echo FAILED)) ==="; fi
[ "$axpass" -gt 0 ] && echo "  !! $axpass assertion(s) marked expect_fail now pass — update meta.json"

# The directory may not exist: when the workload produced no capture at all,
# `pcomm export --csv` writes nothing and $CSV_DIR was never created. The verdict
# still has to be written — a case that failed hardest is exactly the one whose
# result the aggregator must see. Without this the check died with
# "result.json: No such file or directory", which reads as a broken harness
# rather than as a workload that did not run.
mkdir -p "$CSV_DIR"

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
