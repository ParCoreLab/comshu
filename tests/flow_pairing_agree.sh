#!/usr/bin/env bash
# The send->completion pairing exists TWICE, and this is what keeps the two
# honest.
#
# `export --pftrace` pairs work requests in C++ over raw streams, because it has
# to run on a cluster build with no DuckDB. `_wr_flight` in link_sql.hpp pairs
# them in SQL, because the derived layer is SQL by policy and operates on a
# Parquet export where no stream vectors exist. Neither can adopt the other
# without giving up the world it runs in, so they stay separate — 23 non-comment
# lines each, stating one rule twice.
#
# The risk that duplication carries is NOT mechanical drift; it is someone
# changing the rule on one side only. Adding a status filter, folding SRQ
# receives in through ib_recv_cqe, dropping the IBV_WC_RECV exclusion — each
# would leave both sides compiling, both sides producing plausible numbers, and
# the pftrace disagreeing with the panel about what happened. A shared helper
# would not have caught any of them. This does.
#
# Skips loudly (exit 0) when the binary has no DuckDB, the same bargain
# asserts_fire.sh makes with sqlite3.
set -uo pipefail

PCOMM=${1:?usage: flow_pairing_agree.sh <pcomm-binary> <capture-dir>}
CAPTURE=${2:?usage: flow_pairing_agree.sh <pcomm-binary> <capture-dir>}

if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi
if [ ! -d "$CAPTURE" ]; then echo "SKIP: no capture at $CAPTURE"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-flow-agree-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- the C++ side -----------------------------------------------------------
# The count rides the summary line rather than a side channel, so this also
# pins that line's wording as an interface.
if ! "$PCOMM" export "$CAPTURE" --pftrace "$WORK/t.pftrace" > "$WORK/pftrace.out" 2>&1; then
    cat "$WORK/pftrace.out"; echo "FAIL: --pftrace export failed"; exit 1
fi
# A FIXTURE OLDER THAN THE RECORD LAYOUT IS STALE DATA, NOT A BROKEN TOOL, and
# the two must not look alike. `ib_post_send` gained send_flags in place (the
# PRE-ALPHA EXCEPTION in pcomm.fbs), which changes its stride, so a capture
# written before that is refused stream-by-stream — by design, loudly, rather
# than misparsed. This test then has no sends to compare and would report
# "paired 0" as if the rule had broken.
#
# The discriminator is exact: the reader names the condition. Anything else
# producing zero pairs is still a failure.
if grep -q "record stride differs" "$WORK/pftrace.out"; then
    echo "SKIP: $CAPTURE predates the current record layout — re-capture it"
    echo "  (ib_post_send gained send_flags; the stride check refused the stream)"
    exit 0
fi
CPP=$(sed -n 's/.*events, \([0-9]\+\) send->completion flows.*/\1/p' "$WORK/pftrace.out")
if [ -z "$CPP" ]; then
    cat "$WORK/pftrace.out"
    echo "FAIL: no 'N send->completion flows' in the export summary — did the line change?"
    exit 1
fi

# --- the SQL side -----------------------------------------------------------
# Same discriminator as wr_flow_rule.sh: a build without DuckDB names itself and
# exits 0, so grep the message rather than trusting the exit code — and treat
# every other failure as a failure, never as a skip.
"$PCOMM" export "$CAPTURE" --parquet "$WORK/pq" > "$WORK/pq.out" 2>&1
if grep -q "no DuckDB" "$WORK/pq.out"; then
    echo "SKIP: build without DuckDB"; exit 0
fi
"$PCOMM" link "$WORK/pq" > "$WORK/link.out" 2>&1
link_rc=$?
if [ "$link_rc" -ne 0 ] || grep -q "SOME FAILED" "$WORK/link.out"; then
    cat "$WORK/link.out"
    echo "FAIL: link did not complete — a derived query is broken"
    exit 1
fi
SQL=$("$PCOMM" sql "$WORK/pq" "SELECT count(*) FROM wr_flow" 2>/dev/null | tail -1)

# --- they must agree, and must not agree on nothing -------------------------
# The zero check is not padding. Every assertion here passes vacuously on a
# capture whose sends never retire, which is exactly the state a broken rule
# produces — so "they agree" is only worth anything alongside "and they found
# something".
if [ "$CPP" -eq 0 ]; then
    echo "FAIL: fixture paired 0 work requests — it cannot exercise the rule"; exit 1
fi
if [ "$CPP" != "$SQL" ]; then
    echo "FAIL: pairing disagrees — pftrace exporter $CPP, wr_flow $SQL"
    echo "  the two implementations of the (qp_num, wr_id) rule have diverged;"
    echo "  see export_pftrace in pcomm-read/main.cpp and _wr_flight in link_sql.hpp"
    exit 1
fi

# --- the rule itself, clause by clause --------------------------------------
# The count comparison above is NECESSARY AND NOT SUFFICIENT, and this was
# demonstrated rather than assumed: deleting the IBV_WC_RECV exclusion from
# _wr_flight left the count identical and this script green. On a capture where
# a wr_id has one send and one completion, the inner join matches
# min(sends, completions) per key either way — so the count cannot see a send
# paired to the WRONG completion, only a different NUMBER of pairs.
#
# So each clause of the rule is asserted directly on the side that can be
# inspected. A change to the C++ side alone still shows up as a count
# divergence; a change to either side's semantics shows up here.
check_zero() {  # <label> <sql>
    local got
    got=$("$PCOMM" sql "$WORK/pq" "$2" 2>/dev/null | tail -1)
    if [ "${got:-x}" != "0" ]; then
        echo "FAIL: $1 — expected 0, got ${got:-<no result>}"
        echo "  query: $2"
        exit 1
    fi
}
# A receive completion carries the RECEIVER's wr_id and names an ib_post_recv,
# never a send. Pairing one to a send is the failure the exclusion prevents.
check_zero "a send was paired to a RECEIVE completion" \
    "SELECT count(*) FROM wr_flow WHERE (wc_opcode & 128) <> 0"
# A completion cannot precede the send it retires.
check_zero "a work request completed before it was posted" \
    "SELECT count(*) FROM wr_flow WHERE latency_ns < 0"
# One completion retires one work request: no completion may appear twice, and
# no (rank, qp_num, wr_id) generation may be claimed twice.
check_zero "one completion retired more than one send" \
    "SELECT count(*) FROM (SELECT rank, qp_num, t1_ns FROM wr_flow
                           GROUP BY 1,2,3 HAVING count(*) > 1)"

# link_health must also see a clean pairing: an unretired or backwards row means
# the rule dropped work that the counts above cannot reveal, because both sides
# would drop it identically.
#
# DELIBERATELY UNFILTERED BY SEVERITY. Elsewhere the clean predicate is
# `severity = 'defect'`, but this fixture emits no ib_qp_signalling at all, so
# every queue pair's sq_sig_all is NULL and NOTHING may land on the 'note' side.
# Summing both is therefore the stronger claim here: it would catch a
# wr_flight_unretired_selective row appearing where no queue pair ever declared
# anything.
HEALTH=$("$PCOMM" sql "$WORK/pq" \
    "SELECT coalesce(sum(n), 0) FROM link_health WHERE finding LIKE 'wr_flight%'" 2>/dev/null | tail -1)
if [ "${HEALTH:-0}" != "0" ]; then
    "$PCOMM" sql "$WORK/pq" "SELECT * FROM link_health WHERE finding LIKE 'wr_flight%' AND n > 0"
    echo "FAIL: link_health reports $HEALTH unretired/backwards work requests on a clean fixture"
    exit 1
fi

echo "ok: $CPP work requests paired identically by both implementations"
