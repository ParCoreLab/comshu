#!/usr/bin/env bash
# The --csv export's contract, both halves (CR-28, CR-29).
#
#  1. Every row is as wide as its header. Free-text columns — kernel names,
#     object names — were written with a bare %s, and a demangled C++ name is
#     full of commas: `void k<int, float>(int, float)` became four columns.
#  2. A --csv export is for READING. It is decoded (ticks, names, hex), not the
#     raw record layout, so `comshu link` and `comshu ui` must refuse it by
#     name instead of failing table by table in DuckDB's CSV sniffer — while a
#     raw-layout CSV (what the rule tests' fixtures are) still links.
set -uo pipefail
COMSHU=${1:?usage: csv_export_contract.sh <comshu> <writer>}
WRITER=${2:?}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-csvcontract-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail=0

"$WRITER" "$WORK/cap" || { echo "FAIL: writer"; exit 1; }
"$COMSHU" export "$WORK/cap" --csv "$WORK/csv" > "$WORK/export.out" 2>&1 || {
    cat "$WORK/export.out"; echo "FAIL: export"; exit 1; }

# An output directory that exists but cannot be written (CR-64): the export
# used to fprintf into a null FILE* and crash. It must refuse, by name.
mkdir -p "$WORK/ro"; chmod 555 "$WORK/ro"
"$COMSHU" export "$WORK/cap" --csv "$WORK/ro" > "$WORK/ro.out" 2>&1
rc=$?
chmod 755 "$WORK/ro"
if [ "$rc" -ge 128 ] || [ "$rc" -eq 0 ]; then
    cat "$WORK/ro.out"; echo "FAIL: export into an unwritable directory gave rc=$rc"; fail=1
elif ! grep -q 'cannot write' "$WORK/ro.out"; then
    cat "$WORK/ro.out"; echo "FAIL: the refusal did not say what it could not write"; fail=1
else
    echo "ok: an unwritable output directory is refused, not crashed into"
fi

# RFC 4180 width check, in awk-free python so quoting is parsed, not guessed.
python3 - "$WORK/csv" <<'PY' || fail=1
import csv, os, sys
d = sys.argv[1]; bad = 0
for t in ("gpu_kernel", "mem_object"):
    rows = list(csv.reader(open(os.path.join(d, t + ".csv"))))
    widths = {len(r) for r in rows}
    if len(widths) != 1:
        print(f"FAIL: {t}.csv rows have widths {sorted(widths)} — an unquoted string split a row"); bad = 1
    col = rows[0].index("name")
    print(f"ok: {t}.csv name = {rows[1][col]!r}")
k = list(csv.reader(open(os.path.join(d, "gpu_kernel.csv"))))
names = [r[k[0].index("name")] for r in k[1:]]
if names != ["void k<int, float>(int, float)"]:
    print(f"FAIL: kernel name did not round-trip: {names}"); bad = 1
sys.exit(bad)
PY

# The mounting half needs DuckDB; a build without it says so and skips here,
# as every link rule test does.
"$COMSHU" link "$WORK/csv" > "$WORK/probe.out" 2>&1
if grep -q "no DuckDB" "$WORK/probe.out"; then
    echo "SKIP: link half — build without DuckDB"
    [ "$fail" -eq 0 ] && echo "PASS: csv export contract (rows only)"
    exit "$fail"
fi
if "$COMSHU" link "$WORK/csv" > "$WORK/link.out" 2>&1; then
    echo "FAIL: link accepted a decoded --csv export"; fail=1
elif ! grep -q 'export --csv. export' "$WORK/link.out"; then
    cat "$WORK/link.out"; echo "FAIL: link refused it without saying why"; fail=1
else
    echo "ok: link refuses the decoded export by name"
fi

# The raw layout still links: one node_identity row, as the rule fixtures write it.
mkdir -p "$WORK/raw"
cat > "$WORK/raw/node_identity.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,node_hash
0,0,0,0,0,0,0,111
CSV
if ! "$COMSHU" link "$WORK/raw" > "$WORK/raw.out" 2>&1; then
    cat "$WORK/raw.out"; echo "FAIL: a raw-layout CSV no longer links"; fail=1
else
    echo "ok: raw-layout CSV still links"
fi
[ "$fail" -eq 0 ] && echo "PASS: csv export contract"
exit "$fail"
