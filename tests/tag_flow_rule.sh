#!/usr/bin/env bash
# The two-layer tag join, against a capture built to break it.
#
# WHY THIS EXISTS. `tag_flow` is the only join between what the application ASKED
# UCX for (the 600s) and what UCX put on the wire (the 800s), and it rests on one
# measured fact: `uct_send.tag` carries the application's own tag. MN5 job
# 45115011 confirmed it — the same values on both layers, 20 sends each — and
# PLAN-20260815-1540's Stage 3 had been blocked on there being no such key.
#
# The join is a FULL OUTER one because both sides legitimately have rows the
# other does not, and the whole value of the table is in naming which. A fixture
# that only exercised the matching case would pass with an inner join and lose
# both facts the table exists to report.
#
# TWO RANKS, AND THE SAME TAG ON EACH. A tag is a per-rank thing here: rank 0's
# tag 100 and rank 1's tag 100 are two different flows. A join that dropped
# `rank` would merge them and report one flow with double the sends — plausible,
# and wrong in the direction that looks like success.
set -uo pipefail

COMSHU=${1:?usage: tag_flow_rule.sh <comshu-binary>}
if [ ! -x "$COMSHU" ]; then echo "SKIP: no comshu binary at $COMSHU"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-tag-flow-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The UCP layer. status_class: 0 Immediate, 1 Error, 2 Request.
#
# EVERY ROW IS A CLAIM:
#   rank 0 tag 100  two sends, 10 + 20 units    -> matches a UCT flow
#   rank 0 tag 100  one send with status 1      -> EXCLUDED: the call was refused
#   rank 0 tag 200  one send, 4096 units        -> NO tagged UCT op: the 4 MB case
#   rank 1 tag 100  one send, 70 units          -> a DIFFERENT flow from rank 0's
#   rank 0 tag 0    one send, 5 units           -> a genuine tag ZERO, see below
cat > "$WORK/ucx_tag_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,buffer,request,count,tag,status,status_class,door,device
0,2,0,1000,0,1010,0,200,1,0,10,100,0,0,0,65535
0,2,0,1020,0,1030,0,200,1,0,20,100,0,2,0,65535
0,2,0,1040,0,1050,0,200,1,0,9999,100,-3,1,0,65535
0,2,0,1060,0,1070,0,200,1,0,4096,200,0,0,0,65535
1,2,0,1080,0,1090,0,201,1,0,70,100,0,0,0,65535
0,2,0,1100,0,1110,0,200,1,0,5,0,0,0,0,65535
CSV

# The UCT layer. op: 0 am_short, 2 am_bcopy, 10 tag_eager_short,
# 12 tag_eager_zcopy, 13 tag_rndv_zcopy.
#
# EVERY ROW IS A CLAIM:
#   rank 0 tag 100  op 10 + op 12                -> matched, TWO distinct doors
#   rank 0 tag 100  op 2 (am_bcopy), tag 100     -> MUST NOT COUNT: a non-tag
#                                                   door's tag field is 0 by
#                                                   declaration, so a value here
#                                                   is a misread argument slot,
#                                                   and selecting by `tag <> 0`
#                                                   instead of by door swallows it
#   rank 0 tag 100  op 10, status -6             -> EXCLUDED: retried, never sent
#   rank 0 tag 300  op 13                        -> wire_only: UCX's own traffic
#   rank 1 tag 100  op 10 TWICE                   -> one door used twice: uct_ops
#                                                   is 2 and `doors` must stay 1.
#                                                   Without a repeated door,
#                                                   count(op) and
#                                                   count(DISTINCT op) agree and
#                                                   the column is untested — this
#                                                   row exists because the
#                                                   mutation that drops DISTINCT
#                                                   survived the first fixture.
#   rank 0 tag 0    op 10, 5 bytes               -> the genuine tag zero: it must
#                                                   MATCH, which a `tag <> 0`
#                                                   filter would make impossible
cat > "$WORK/uct_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,payload,header,remote_addr,tag,ep,iface,status,op,iovcnt,pad0,pad1
0,2,0,2000,0,2010,0,10,0,0,100,20,10,0,10,0,0,0
0,2,0,2020,0,2030,0,20,0,0,100,20,10,0,12,1,0,0
0,2,0,2040,0,2050,0,777,0,0,100,20,10,0,2,0,0,0
0,2,0,2060,0,2070,0,888,0,0,100,20,10,-6,10,0,0,0
0,2,0,2080,0,2090,0,4,0,0,300,20,10,0,13,1,0,0
1,2,0,2100,0,2110,0,70,0,0,100,21,11,0,10,0,0,0
1,2,0,2140,0,2150,0,30,0,0,100,21,11,0,10,0,0,0
0,2,0,2120,0,2130,0,5,0,0,0,20,10,0,10,0,0,0
CSV

"$COMSHU" link "$WORK" > "$WORK/link.out" 2>&1
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
    got=$("$COMSHU" sql "$WORK" "$2" 2>/dev/null | tail -1 | tr -d '"')
    if [ "$got" != "$3" ]; then
        echo "FAIL: $1"; echo "  expected: $3"; echo "  got:      ${got:-<no result>}"
        echo "  query:    $2"; fail=1
    fi
    return 0
}

# The whole relation in one value. Every defect this file is about changes it:
# an inner join loses two rows, dropping `rank` merges rank 1's flow into rank
# 0's, selecting the UCT side by `tag <> 0` loses the tag-zero match and gains
# the am_bcopy, and counting the error rows inflates two columns.
expect "the flow" \
    "SELECT string_agg(rank||':'||tag||':'||status||':'||ucp_sends||'/'||ucp_units||':'||uct_ops||'/'||uct_payload||':'||doors, ' ' ORDER BY rank, status, tag) FROM tag_flow" \
    "0:200:no_tagged_wire_op:1/4096:0/0:0 0:0:tag_matched:1/5:1/5:1 0:100:tag_matched:2/30:2/30:2 0:300:wire_only:0/0:1/4:1 1:100:tag_matched:1/70:2/100:1"

# --- the three statuses, one at a time --------------------------------------
#
# A UCP send with no tagged wire operation is NOT lost traffic and the table must
# not imply it is: the wire operations exist, they carry no tag, because the
# message took a door that has none. On job 45115011 this is where the 4 MB size
# sits — above the offload ceiling, sent as an AM the peer pulls — while the
# 64 KB size below it is tag_matched. An inner join erases this row and with it
# the only visible sign that a protocol boundary was crossed.
expect "a UCP send with no tagged wire op is kept and named" \
    "SELECT ucp_sends||'/'||uct_ops FROM tag_flow WHERE rank = 0 AND tag = 200" "1/0"

# The other direction: the wire carries more than the application asked for.
# Job 45115011 shows 19 four-byte operations per rank on a tag no application
# call produced. Dropping these makes the wire look exactly as busy as the
# application, which is the illusion this whole layer exists to break.
expect "a tagged wire op with no UCP send is kept and named" \
    "SELECT ucp_sends||'/'||uct_ops FROM tag_flow WHERE rank = 0 AND tag = 300" "0/1"

# --- the two filters that are easy to get subtly wrong -----------------------
#
# TAG ZERO IS A REAL TAG. The UCT side is selected by DOOR (ops 10-13), not by
# `tag <> 0`, because UctSend declares `tag` to be 0 on every non-tag door — so
# a `tag <> 0` filter throws away a genuine tag-0 message, and silently. This is
# the clause that catches the shortcut.
expect "a genuine tag of zero still matches" \
    "SELECT status||':'||uct_ops FROM tag_flow WHERE rank = 0 AND tag = 0" "tag_matched:1"

# The converse of the same rule: an am_bcopy carrying a non-zero tag is a
# misread argument slot, not a tagged send, and selecting by door excludes it by
# construction. 777 is chosen so leaking it into uct_payload is unmissable.
expect "a non-tag door carrying a tag is not counted" \
    "SELECT count(*) FROM tag_flow WHERE uct_payload >= 777" "0"

# UCS_ERR_NO_RESOURCE moved nothing, and neither did the rejected UCP call.
# 9999 and 888 are chosen so either leaking is loud.
expect "rejected and retried operations are not counted" \
    "SELECT ucp_units||'/'||uct_payload FROM tag_flow WHERE rank = 0 AND tag = 100" "30/30"

# --- the key is a PAIR ------------------------------------------------------
#
# Rank 0 and rank 1 both use tag 100 and they are different flows. A join keyed
# on the tag alone reports one flow with three sends instead of two flows — the
# same "a wire id is only unique with its scope" rule the whole derived layer
# opens with, arriving on a third kind of id.
expect "one tag on two ranks is two flows" \
    "SELECT count(*)||':'||string_agg(rank||'='||ucp_sends, ',' ORDER BY rank) FROM tag_flow WHERE tag = 100" \
    "2:0=2,1=1"

# `doors` counts DISTINCT doors, so it says a flow crossed a protocol boundary
# without needing a row per operation. Rank 0's tag 100 went out as both an
# eager short and an eager zcopy.
expect "a flow that used two doors says so" \
    "SELECT doors FROM tag_flow WHERE rank = 0 AND tag = 100" "2"

# And the converse, which is what makes the column mean "doors" rather than
# "operations": rank 1 sent twice through the SAME door, so uct_ops is 2 and
# doors is 1. A count(op) that forgot DISTINCT reads 2 here and is
# indistinguishable from rank 0's genuine two-door flow.
expect "one door used twice is still one door" \
    "SELECT uct_ops||'/'||doors FROM tag_flow WHERE rank = 1 AND tag = 100" "2/1"

[ "$fail" -eq 0 ] || exit 1
echo "ok: the two-layer tag join holds on a capture built to break it"
