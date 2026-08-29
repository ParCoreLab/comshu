#!/usr/bin/env bash
# The probe -> msg_recv join, against a capture built to break it.
#
# WHY THIS EXISTS. `ucp_tag_msg_recv_nb` takes a `ucp_tag_message_h` and no tag,
# so UcxTagRecv writes tag and tag_mask as 0 on that door and carries the message
# handle instead. The tag is recoverable — the probe that produced the handle was
# handed it in `ucp_tag_recv_info_t` — which is why ucx_hooks.hpp hooks the two
# TOGETHER and why UcxTagRecv's declaration instructs a reader to "join it to
# ucx_tag_probe.message to recover the tag AND the true delivered length". Until
# `tag_recv_resolved` existed nothing performed that join, so the instruction
# described a table that was never built: a msg_recv row had a tag in principle
# and not in practice.
#
# THE RULE IS TEMPORAL AND THAT IS THE WHOLE POINT. `ucp_tag_message_h` is a
# pointer into UCX's request mpool and is RECYCLED — observed directly while the
# records were designed, with UCX printing its own mpool-reuse warning in the
# same run. A plain equality join pairs a receive with a long-dead probe as soon
# as the pool wraps, and the result is indistinguishable from a correct pairing.
# No hardware capture reproduces that on demand, which is exactly why the rule is
# pinned here with a fixture instead of being left to a lucky run.
#
# Every defect this file is about is a join that still returns rows:
#   - equality without recency  -> the stale probe wins, or both match and the
#                                  receive is double-counted
#   - `>=` instead of `<=`      -> a receive matches a probe from its future
#   - dropping `worker`         -> another worker's identical handle matches
#   - dropping `rank`           -> rank 1's probe answers rank 0's receive
#   - an inner join             -> the unprobed receives vanish silently, which
#                                  reads as "this rank posted none"
set -uo pipefail

PCOMM=${1:?usage: tag_recv_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-tag-recv-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The probes. EVERY ROW IS A CLAIM:
#   r0 t=1000 w=10 msg=500 tag=111  -> STALE. The handle is reused at t=2000, so
#                                      this must never answer a later receive.
#   r0 t=2000 w=10 msg=500 tag=222  -> LIVE. The most recent at or before t=3000.
#   r0 t=4000 w=10 msg=501 tag=333  -> IN THE FUTURE of the t=1500 receive, so it
#                                      must not match it. Catches a flipped
#                                      comparison, which otherwise looks fine.
#   r0 t=1200 w=99 msg=503 tag=444  -> a DIFFERENT WORKER holding the same kind
#                                      of handle. Catches a dropped worker key.
#   r1 t=2500 w=20 msg=500 tag=555  -> rank 1's own. Catches a dropped rank key,
#                                      which would let it answer rank 0.
cat > "$WORK/ucx_tag_probe.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,worker,message,sender_tag,length,tag,tag_mask,removed
0,2,0,1000,0,1010,0,10,500,111,48,0,0,0
0,2,0,2000,0,2010,0,10,500,222,64,0,0,1
0,2,0,4000,0,4010,0,10,501,333,16,0,0,1
0,2,0,1200,0,1210,0,99,503,444,8,0,0,1
1,2,0,2500,0,2510,0,20,500,555,128,0,0,1
CSV

# The receives. door: 0 nbx, 4 msg_recv_nb. status_class: 0 Immediate, 1 Error.
#
# EVERY ROW IS A CLAIM:
#   r0 t=1500 msg=501 -> no_probe: its only probe happens at t=4000, later
#   r0 t=3000 msg=500 -> matches the LIVE probe (222/64), never the stale one
#   r0 t=3100 msg=502 -> no_probe: no probe for this handle at all
#   r0 t=3200 msg=503 -> no_probe: the only probe for 503 is on worker 99
#   r0 t=3300 door=0  -> EXCLUDED: not the handle-keyed door. It already carries
#                        its own tag and has no message handle to join on.
#   r0 t=3400 sc=1    -> EXCLUDED: the call was refused
#   r0 t=3500 msg=500 -> a SECOND receive of the same handle, and it must produce
#                        its OWN row. A receive has no unique key, so this is
#                        what proves the manufactured `seq` keeps two otherwise
#                        identical receives apart instead of collapsing them.
#   r1 t=3000 msg=500 -> matches rank 1's probe (555/128), not rank 0's
cat > "$WORK/ucx_tag_recv.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,worker,buffer,request,count,tag,tag_mask,message,status,status_class,door
0,2,0,1500,0,1510,0,10,1,0,100,0,0,501,0,0,4
0,2,0,3000,0,3010,0,10,1,0,4096,0,0,500,0,0,4
0,2,0,3100,0,3110,0,10,1,0,200,0,0,502,0,0,4
0,2,0,3200,0,3210,0,10,1,0,300,0,0,503,0,0,4
0,2,0,3300,0,3310,0,10,1,0,600,77,0,0,0,0,0
0,2,0,3400,0,3410,0,10,1,0,700,0,0,504,-3,1,4
0,2,0,3500,0,3510,0,10,1,0,4096,0,0,500,0,0,4
1,2,0,3000,0,3010,0,20,1,0,500,0,0,500,0,0,4
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
        echo "FAIL: $1"; echo "  expected: $3"; echo "  got:      ${got:-<no result>}"
        echo "  query:    $2"; fail=1
    fi
    return 0
}

# The whole relation in one value. Every defect above changes it: taking the
# stale probe swaps 222 for 111, a flipped comparison turns 501 into a match,
# dropping worker or rank turns 503 or rank 0's 500 into someone else's tag, an
# inner join loses all three no_probe rows, and collapsing on the handle loses
# one of the two t=3000/t=3500 rows.
#
# The two matched rank-0 rows are identical in every published column, so their
# order between themselves carries no information and the aggregate is stable
# without tie-breaking on a column the table does not expose.
expect "the resolution" \
    "SELECT string_agg(rank||':'||message||':'||status||':'||coalesce(sender_tag::VARCHAR,'-')||':'||offered||':'||coalesce(delivered::VARCHAR,'-'), ' ' ORDER BY rank, message, status) FROM tag_recv_resolved" \
    "0:500:probe_matched:222:4096:64 0:500:probe_matched:222:4096:64 0:501:no_probe:-:100:- 0:502:no_probe:-:200:- 0:503:no_probe:-:300:- 1:500:probe_matched:555:500:128"

# --- the recency rule, isolated ---------------------------------------------
# Stated on its own because it is the one an equality join gets wrong while
# still returning a plausible row. 111 is the stale probe's tag; seeing it here
# means the join stopped honouring "at or before, most recent".
expect "a recycled handle resolves to the LIVE probe, not the dead one" \
    "SELECT DISTINCT sender_tag FROM tag_recv_resolved WHERE rank=0 AND message=500" \
    "222"

# One row per receive, never one per (receive, probe). With two probes carrying
# handle 500 an equality join returns four rows here instead of two.
expect "two receives of one handle stay two rows" \
    "SELECT count(*) FROM tag_recv_resolved WHERE rank=0 AND message=500" \
    "2"

# --- the excluded doors -----------------------------------------------------
# A tagged receive that already knows its own tag has nothing to recover, and
# putting it here would invite a reader to treat a NULL sender_tag as a defect.
expect "a non-handle door produces no row" \
    "SELECT count(*) FROM tag_recv_resolved WHERE offered=600" \
    "0"
expect "a refused receive produces no row" \
    "SELECT count(*) FROM tag_recv_resolved WHERE offered=700" \
    "0"

# --- the delivered length ---------------------------------------------------
# UcxTagProbe.length is the one TRUE byte count in the UCX family; UcxTagRecv
# .count is capacity OFFERED and its own declaration forbids adding it to a
# bytes total. The join is what puts the true number beside the receive, and
# they must not be equal here or the fixture would not distinguish them.
expect "delivered is the probe's length, not the receive's capacity" \
    "SELECT sum(delivered) FROM tag_recv_resolved WHERE status='probe_matched'" \
    "256"

if [ "$fail" -ne 0 ]; then
    echo
    echo "tag_recv_rule: FAILED — the probe/msg_recv join does not follow its rule"
    exit 1
fi
echo "tag_recv_rule: PASS — the tag a msg_recv did not carry is recovered, temporally"
