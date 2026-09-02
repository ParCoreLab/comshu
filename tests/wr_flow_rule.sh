#!/usr/bin/env bash
# The send->completion pairing rule, against a capture built to break it.
#
# WHY THIS EXISTS SEPARATELY FROM flow_pairing_agree.sh. That script compares
# the C++ and SQL implementations on a real fixture and asserts they agree. It
# is necessary and it is not sufficient, which was demonstrated rather than
# assumed: deleting the IBV_WC_RECV exclusion from _wr_flight left it green
# TWICE — once because the pair count is min(sends, completions) per key and did
# not move, and again because ib_pingpong's receive wr_ids never collide with
# its send wr_ids, so the extra completions had no send to be mispaired to. A
# fixture that cannot express the defect cannot test for it.
#
# So this one builds a capture where every clause of the rule has something to
# get wrong, on the same reasoning as hpc/suite/asserts_fire.sh: an assertion
# that has been relaxed into silence looks exactly like one that passes.
#
# Three CSVs are enough — mount_raw_tables creates every other raw table empty
# with the right columns, so the derived layer builds on a capture that contains
# only what this test is about.
set -uo pipefail

PCOMM=${1:?usage: wr_flow_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-wr-rule-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# Two anchors one tick-per-ns apart, so wall-clock ns EQUALS ticks and every
# expected latency below is readable as the arithmetic it is.
cat > "$WORK/clock_anchor.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,domain,local_ticks,wall_ns,mono_ns
0,0,0,1000,0,0,0,0,1000,1000,1000
0,0,0,1000000,0,0,0,0,1000000,1000000,1000000
CSV

# ONE CONCERN PER QUEUE PAIR, because the signalling guard is per-QP: a queue
# pair that posts more work requests than it completes is refused WHOLESALE, so
# a fixture that put every case on one QP would test nothing but the guard.
#
# qp 7 — BALANCED (4 sends, 4 send-side completions), so it is pairable and the
# matching rule itself is under test:
#   wr_id 100  one send, one completion        -> pairs, latency 1000
#              plus a RECEIVE completion at 2500, deliberately placed BEFORE the
#              send completion at 3000: if the IBV_WC_RECV exclusion is dropped
#              the receive becomes generation 1 and the latency reads 500.
#              (Receives are excluded from the balance count too, so adding one
#              does not disturb the guard.)
#   wr_id 300  TWO sends, two completions      -> FIFO, latencies 1000 and 2500
#   wr_id 400  a completion BEFORE its send    -> dropped, counted backwards
#
# qp 8 — UNBALANCED (2 sends, 1 completion), the unsignalled-WR signature. The
# guard must refuse the whole queue pair; without it, this pairs and adds a
# latency of 200.
cat > "$WORK/ib_post_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,qp,wr_id,remote_addr,rkey,length,qp_num,send_flags,opcode,num_sge
0,2,0,2000,0,2010,0,1,100,0,0,64,7,2,0,1
0,2,0,6000,0,6010,0,1,300,0,0,64,7,2,0,1
0,2,0,6500,0,6510,0,1,300,0,0,64,7,2,0,1
0,2,0,8000,0,8010,0,1,400,0,0,64,7,2,0,1
0,2,0,2000,0,2010,0,1,500,0,0,64,8,0,0,1
0,2,0,2100,0,2110,0,1,500,0,0,64,8,0,0,1
0,2,0,3000,0,3010,0,1,600,0,0,64,9,0,0,1
0,2,0,3100,0,3110,0,1,600,0,0,64,9,2,0,1
0,2,0,4000,0,4010,0,1,700,0,0,64,10,2,0,1
0,2,0,4100,0,4110,0,1,701,0,0,64,10,2,0,1
CSV

# opcode 128 is IBV_WC_RECV; opcode 0 is IBV_WC_SEND.
cat > "$WORK/ib_poll_cq.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,cq,wr_id,byte_len,qp_num,status,opcode
0,2,0,2500,0,2510,0,2,100,64,7,0,128
0,2,0,3000,0,3010,0,2,100,64,7,0,0
0,2,0,7000,0,7010,0,2,300,64,7,0,0
0,2,0,9000,0,9010,0,2,300,64,7,0,0
0,2,0,1500,0,1510,0,2,400,64,7,0,0
0,2,0,2200,0,2210,0,2,500,64,8,0,0
0,2,0,3400,0,3410,0,2,600,64,9,0,0
0,2,0,4500,0,4510,0,2,700,64,10,0,0
CSV

# send_flags is column 14 of ib_post_send above; bit 1 (value 2) is
# IBV_SEND_SIGNALED. qp 8 signals NOTHING and so cannot be paired at all; qp 9
# signals only its second work request — the completion at 3400 belongs to that
# send at 3100, not to the unsignalled one at 3000, so the right latency is 300
# and pairing by position would say 400. qp 10 signals both and completes one.
#
# qp 7 declares it signals everything; qp 8 declares it does not. The counts
# already decide pairability — this is what lets link_health name the CAUSE of
# qp 8's mismatch instead of leaving it as "unknown".
cat > "$WORK/ib_qp_signalling.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,qp,qp_num,sq_sig_all
0,0,0,0,0,0,0,1,7,1
0,0,0,0,0,0,0,2,8,0
0,0,0,0,0,0,0,3,9,0
0,0,0,0,0,0,0,4,10,0
CSV

# qp 9 is selectively signalling AND said which work requests it signalled, so
# it IS pairable: the completion at 3400 belongs to the signalled send at 3100,
# not to the unsignalled one at 3000. Pairing by position instead would give a
# latency of 400 measured from the wrong send; the right answer is 300.
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
    # `sql` emits CSV, so a value containing commas comes back quoted. Strip
    # them rather than expecting them: the quoting is the transport's, not the
    # answer's.
    got=$("$PCOMM" sql "$WORK" "$2" 2>/dev/null | tail -1 | tr -d '"')
    if [ "$got" != "$3" ]; then
        echo "FAIL: $1"
        echo "  expected: $3"
        echo "  got:      ${got:-<no result>}"
        echo "  query:    $2"
        fail=1
    fi
}

# The whole rule in one value: which work requests paired, and how long each
# took. Every defect below changes this string.
#   - pairing the receive completion adds a fourth latency
#   - matching newest-first instead of oldest-first gives 500 and 3000
#   - keeping the backwards pair adds a negative
expect "paired latencies" \
    "SELECT string_agg(latency_ns, ',' ORDER BY latency_ns) FROM wr_flow" \
    "300,1000,1000,2500"

# Stated again as an invariant rather than a value, because this is the clause
# that survived two attempts to test it by counting.
expect "no send paired to a RECEIVE completion" \
    "SELECT count(*) FROM wr_flow WHERE (wc_opcode & 128) <> 0" "0"

expect "the backwards pair was dropped and counted" \
    "SELECT n FROM link_health WHERE finding = 'wr_flight_backwards'" "1"

# qps 8, 9 and 10 all post more than they complete, and ALL THREE declared
# sq_sig_all = 0. So the capture can name the cause of every one of them and
# nothing is left unexplained — which is the point of the row being zero here
# rather than absent.
expect "no unpairable queue pair is left without a cause" \
    "SELECT n FROM link_health WHERE finding = 'wr_flight_unexplained_qps'" "0"

# The same three, on the side of the rubric that is not a defect. They declared
# they would not signal every work request; the mismatch is the declaration
# coming true, not a failure of the link.
expect "the cause of the refusal is named, not guessed" \
    "SELECT n FROM link_health WHERE finding = 'wr_flight_selective_signalling_qps'" "3"

# THE SPLIT IS THE WHOLE POINT. Six sends never reached the flight lane, and
# exactly ONE of them is a defect: qp 7's backwards work request, on the only
# queue pair that declared it signals everything. The other five are qp 8's two,
# qp 9's unsignalled one and qp 10's two — posts for which no completion was
# ever going to be emitted, because their queue pairs said so at creation.
#
# Before the severity column this read 6, and a reader chasing it found five
# posts that were working as designed. That ratio is 1:5 on a ten-row fixture;
# on job 45330280 it was 0:69,470.
expect "unretired sends with no declared cause" \
    "SELECT n FROM link_health WHERE finding = 'wr_flight_unretired'" "1"

expect "unretired sends the queue pair already accounted for" \
    "SELECT n FROM link_health WHERE finding = 'wr_flight_unretired_selective'" "5"

# The split RECLASSIFIES, it does not lose rows: the two counts must still sum
# to every post the flight lane could not pair. Without this, narrowing the
# defect row's WHERE clause to zero would look like a fix.
expect "the two unretired rows account for every unpaired post" \
    "SELECT (SELECT count(*) FROM ib_post_send) - (SELECT count(*) FROM wr_flow)
       - (SELECT sum(n) FROM link_health WHERE finding LIKE 'wr_flight_unretired%')" "0"

# Severity is not decoration: of the four wr_flight rows that fire on this
# fixture, exactly two must survive the clean predicate. Scoped to wr_flight%
# rather than to the whole rubric because this fixture ships no ib_modify_qp and
# no mem_object, so unresolved_sends and unattributed_recv_completions fire too
# — correctly, and for reasons this file does not test.
expect "the clean predicate keeps qp 7's backwards send and drops the declared ones" \
    "SELECT string_agg(finding, ',' ORDER BY finding) FROM link_health
      WHERE n > 0 AND severity = 'defect' AND finding LIKE 'wr_flight%'" \
    "wr_flight_backwards,wr_flight_unretired"

expect "and the declared ones are present, on the other side" \
    "SELECT string_agg(finding, ',' ORDER BY finding) FROM link_health
      WHERE n > 0 AND severity = 'note' AND finding LIKE 'wr_flight%'" \
    "wr_flight_selective_signalling_qps,wr_flight_unretired_selective"

# The Timeline reads the same relation through timeline_span; a lane that
# disagrees with the table under it is the drift this whole file guards.
expect "the flight lane carries exactly the paired work requests" \
    "SELECT count(*) FROM timeline_span WHERE stream = 'ib_wr_flight'" "4"

[ "$fail" -eq 0 ] || exit 1
echo "ok: pairing rule holds on a capture built to break it"
