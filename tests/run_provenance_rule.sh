#!/usr/bin/env bash
# run_provenance: the invocation record, with its handles resolved.
#
# The RECORD is trivial — six uint32s, one row per rank — and the interesting
# part is entirely in the join. `invocation` in Parquet holds five interned
# handles and answers nothing on its own; run_provenance is what turns them back
# into the argv, the environment, the build id and the list of mapped .so files
# that the whole point of the record was to carry.
#
# WHAT THIS FIXTURE IS BUILT TO BREAK, in the same spirit as wr_flow_rule.sh:
#
#   * handle scope. Handles restart at 1 in every domain of every rank, so
#     handle 1 on rank 0 and handle 1 on rank 1 are DIFFERENT strings. A join
#     that drops `rank` still passes any single-rank test and silently
#     cross-attributes the moment there are two.
#   * domain scope. Handle 1 in InternDomain::KernelName is not handle 1 in
#     InternDomain::String. Dropping `domain` puts a kernel name in the argv.
#   * ambiguity. A REPLAYED capture nests one rank-* directory per pass and both
#     load as rank 0 with independent dictionaries, so one handle can name two
#     strings. A direct join would emit a row per candidate and multiply this
#     table; the rule is that it resolves to NULL and the RANK'S ROW SURVIVES.
#   * absence. kNullHandle (0) means the engine could not fill that field —
#     an unreadable /proc, an empty allowlist — and must read as NULL without
#     taking the row with it.
#
# Only two CSVs, because mount_raw_tables creates every other raw table empty
# with the right columns.
set -uo pipefail

PCOMM=${1:?usage: run_provenance_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-prov-rule-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# Three ranks, and every one of them exercises a different failure:
#
#   rank 0  fully populated, handles 1..5
#   rank 1  THE SAME HANDLE NUMBERS, different strings — the scope clause
#   rank 2  argv handle 9 is ambiguous (two texts) and env handle is 0
cat > "$WORK/invocation.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,argv,env,providers,build,images,pid
0,0,0,0,0,0,0,1,2,3,4,5,1000
1,0,0,0,0,0,0,1,2,3,4,5,1001
2,0,0,0,0,0,0,9,0,3,4,5,1002
CSV

# domain 2 is InternDomain::String (Pc, Pointer, String, KernelName, TypeName).
# Handle 1 in domain 3 is a decoy: it is the same rank and the same handle
# number as rank 0's argv, and a join that forgets `domain` picks it up.
# Handle 9 on rank 2 is claimed by two strings — the replayed-capture shape.
cat > "$WORK/intern_text.csv" <<'CSV'
rank,domain,handle,text
0,2,1,/opt/lmp -in in.lj
0,2,2,SLURM_NNODES=4
0,2,3,"ibverbs,pcomm-engine"
0,2,4,commit=abc123 type=Release
0,2,5,/build/libpcomm_engine.so 990704 1788340359
0,3,1,SHOULD_NOT_APPEAR_a_kernel_name
1,2,1,/opt/lmp -in in.rhodo
1,2,2,SLURM_NNODES=8
1,2,3,"ibverbs,pcomm-engine"
1,2,4,commit=abc123 type=Release
1,2,5,/build/libpcomm_engine.so 990704 1788340359
2,2,3,"ibverbs,pcomm-engine"
2,2,4,commit=abc123 type=Release
2,2,5,/build/libpcomm_engine.so 990704 1788340359
2,2,9,/opt/lmp -in one.thing
2,2,9,/opt/other -in another.thing
CSV

"$PCOMM" link "$WORK" > "$WORK/link.out" 2>&1
link_rc=$?
if grep -q "no DuckDB" "$WORK/link.out"; then
    echo "SKIP: build without DuckDB"; exit 0
fi
if [ "$link_rc" -ne 0 ] || grep -q "SOME FAILED" "$WORK/link.out"; then
    cat "$WORK/link.out"
    echo "FAIL: link did not complete — a derived query is broken"
    exit 1
fi

fail=0
expect() {  # <label> <sql> <expected>
    local got
    got=$("$PCOMM" sql "$WORK" "$2" 2>/dev/null | tail -1 | tr -d '"')
    if [ "$got" != "$3" ]; then
        echo "FAIL: $1"
        echo "  expected: $3"
        echo "  got:      ${got:-<no result>}"
        echo "  query:    $2"
        fail=1
    fi
}

# One row per rank, no more. A join that multiplied on rank 2's ambiguous handle
# would report 4 here, and every per-rank aggregate downstream would be wrong.
expect "one row per rank, and the ambiguous one did not multiply" \
    "SELECT count(*) FROM run_provenance" "3"

# THE SCOPE CLAUSE. Same handle number, two ranks, two different command lines.
# Dropping `rank` from the join produces both strings for both ranks, which the
# count above would also catch — this names the value, so a failure says which
# way it went wrong.
expect "handle 1 means a different argv on each rank" \
    "SELECT string_agg(argv, '|' ORDER BY rank) FROM run_provenance WHERE rank < 2" \
    "/opt/lmp -in in.lj|/opt/lmp -in in.rhodo"

# THE DOMAIN CLAUSE. rank 0 handle 1 also exists in InternDomain::KernelName.
expect "the KernelName decoy at the same handle is not picked up" \
    "SELECT count(*) FROM run_provenance WHERE argv LIKE 'SHOULD_NOT_APPEAR%'" "0"

# THE AMBIGUITY CLAUSE, both halves: the field resolves to NULL rather than to
# an arbitrary one of the two, AND the rank keeps its row with the fields that
# were unambiguous.
expect "an ambiguous handle resolves to NULL, not to a guess" \
    "SELECT count(*) FROM run_provenance WHERE rank = 2 AND argv IS NULL" "1"

expect "and the rest of that rank's row survives" \
    "SELECT build FROM run_provenance WHERE rank = 2" \
    "commit=abc123 type=Release"

# THE ABSENCE CLAUSE. Handle 0 is kNullHandle — the engine could not fill the
# field — and must read as NULL rather than as the empty string or a dropped row.
expect "a null handle reads as NULL" \
    "SELECT count(*) FROM run_provenance WHERE rank = 2 AND env IS NULL" "1"

# The fields a reader actually reaches for. Stated as values rather than as
# "not null", because a join that resolved every handle to the same string
# would satisfy the weaker form.
expect "the build id is carried verbatim" \
    "SELECT count(DISTINCT build) FROM run_provenance" "1"

expect "providers and images resolve on the fully-populated rank" \
    "SELECT providers || ' :: ' || images FROM run_provenance WHERE rank = 0" \
    "ibverbs,pcomm-engine :: /build/libpcomm_engine.so 990704 1788340359"

# pid comes off the record rather than the dictionary, so it is the control:
# if this ever goes wrong the fault is in the record, not in the join.
expect "pid is carried off the record itself" \
    "SELECT string_agg(pid, ',' ORDER BY rank) FROM run_provenance" "1000,1001,1002"

[ "$fail" -eq 0 ] || exit 1
echo "ok: run_provenance resolves per (rank, domain, handle) and refuses to guess"
