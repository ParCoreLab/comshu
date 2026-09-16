#!/usr/bin/env bash
# What `pcomm sql` accepts as a query — the CLI's contract, not the SQL's.
#
# WHY THIS EXISTS. `pcomm sql` wraps its argument in `COPY (<q>) TO`, which is
# what makes the command work and also what makes two ordinary things a person
# writes into a parse error:
#
#   SELECT 1;         the ';' lands inside the parens
#   SELECT 1 -- why   the comment eats the wrapper's own ')', and the error
#                     says "syntax error at end of input", pointing at nothing
#
# The second one matters more than it looks: every .sql file under
# tests/hpc/*/asserts is annotated with exactly that kind of trailing comment,
# and tests/hpc/suite/sql_check.sh carries a python splicer whose first job is
# to strip them back off before handing the body to this command.
#
# RECORDED AS A FINDING FIRST, then measured. PLAN-20260902-0300 finding 13
# listed THREE warts: a leading comment, a trailing semicolon, and `params` as a
# leading CTE breaking an assertion that carries its own WITH. Only one and a
# half of those were real — a leading comment and an own `WITH` both work today
# and are asserted below so they stay working, and the `params` splice is
# sql_check.sh's job rather than this command's. Writing the test is what
# separated them.
#
# The fixture is one CSV. Nothing here is about the data.
set -uo pipefail

PCOMM=${1:?usage: sql_cli_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-sqlcli-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/node_identity.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,node_hash
0,0,0,0,0,0,0,111
1,0,0,0,0,0,0,111
2,0,0,0,0,0,0,222
CSV

if ! "$PCOMM" sql "$WORK" "SELECT 1" >/dev/null 2>&1; then
    # Distinguish "no DuckDB in this build" from "the command is broken": the
    # first is a legitimate configuration (MN5 builds with PCOMM_DUCKDB=OFF) and
    # the second is what this file exists to catch.
    # Captured into a variable rather than piped straight into grep, and that is
    # the whole fix: `set -o pipefail` at the top of this file makes a pipeline
    # report the FAILING left-hand status even when grep matches. So this probe
    # always looked like a miss, and every DuckDB-less build — MN5's, and any
    # default `cmake -B build` — reported a hard FAIL instead of the skip the
    # comment above promises. The sibling rules never hit it because they grep a
    # file (capture_integrity_rule.sh) instead of a failing command.
    probe=$("$PCOMM" sql "$WORK" "SELECT 1" 2>&1)
    if printf '%s' "$probe" | grep -q "no DuckDB"; then
        echo "SKIP: build without DuckDB"; exit 0
    fi
    echo "FAIL: pcomm sql cannot run the simplest possible query"
    printf '%s\n' "$probe" | head -5
    exit 1
fi

fail=0
# Every form below must produce the SAME answer as the plain one. Comparing the
# VALUE rather than the exit status is deliberate: a wrapper defect that
# silently truncated the query could still exit 0.
accepts() {  # <label> <query>
    local got
    got=$("$PCOMM" sql "$WORK" "$2" 2>&1 | tail -1 | tr -d '"')
    if [ "$got" != "3" ]; then
        echo "FAIL: pcomm sql rejects $1"
        echo "  expected the same answer as the plain form: 3"
        echo "  got:      ${got:-<no result>}"
        fail=1
    fi
}

accepts "a plain query"              "SELECT count(*) FROM node_identity"
accepts "a trailing semicolon"       "SELECT count(*) FROM node_identity;"
accepts "a semicolon and a newline"  "SELECT count(*) FROM node_identity;
"
accepts "several trailing semicolons and space" \
                                     "SELECT count(*) FROM node_identity ;  ; "

# The one that reads as "syntax error at end of input" when it breaks, because
# the '--' runs past the wrapper's closing paren. Fixed by putting that paren on
# its own line, which is why the newline-terminated form below always worked and
# this one did not.
accepts "a trailing line comment"    "SELECT count(*) FROM node_identity -- why"
accepts "a trailing comment and a semicolon" \
                                     "SELECT count(*) FROM node_identity; -- why"

# Asserted because finding 13 claimed these were broken and measurement said
# otherwise. A test is how a corrected claim stays corrected.
accepts "a leading comment"          "-- what this counts
SELECT count(*) FROM node_identity"
accepts "a leading and a trailing comment" \
                                     "-- lead
SELECT count(*) FROM node_identity -- tail"
accepts "a query carrying its own WITH" \
                                     "WITH n AS (SELECT * FROM node_identity) SELECT count(*) FROM n"
accepts "a WITH plus a leading comment and a trailing semicolon" \
                                     "-- lead
WITH n AS (SELECT * FROM node_identity) SELECT count(*) FROM n;"

# A semicolon INSIDE a string literal is not a statement terminator and must
# survive. This is the clause that makes the strip safe rather than merely
# convenient: a rule that chased semicolons anywhere would corrupt this query.
got=$("$PCOMM" sql "$WORK" "SELECT 'a;b' AS s" 2>&1 | tail -1 | tr -d '"')
if [ "$got" != "a;b" ]; then
    echo "FAIL: a semicolon inside a string literal was not preserved"
    echo "  expected: a;b"
    echo "  got:      ${got:-<no result>}"
    fail=1
fi

# A genuinely broken query must still FAIL. Without this the whole file could be
# satisfied by a wrapper that swallowed everything — the vacuous-pass shape.
if "$PCOMM" sql "$WORK" "SELECT FROM WHERE" >/dev/null 2>&1; then
    echo "FAIL: a malformed query was accepted — the normalisation is too eager"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "ok: pcomm sql accepts what a person writes and still refuses what is broken"
