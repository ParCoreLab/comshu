#!/usr/bin/env bash
# nccl_execution / nccl_peer_traffic: the profiler-plugin layer, joined.
#
# The RECORDS are plain — four interned handles and some integers — and every
# interesting failure is in the two joins above them:
#
#   nccl_coll_exec  --nccl_comm_hash-->  nccl_comm       (cross-LAYER)
#   nccl_proxy_op   --parent_event_id-->  nccl_coll_exec  (causal)
#
# WHAT THIS FIXTURE IS BUILT TO BREAK. Each rank below carries one specific
# trap, and none of them is hypothetical:
#
#   * THE ZERO HASH. nccl_comm.nccl_comm_hash is 0 when NCCL's commHash is not
#     derivable — ncclCommSplit and the grow path mix theirs from a parent. If
#     the join does not exclude 0, every such communicator on a rank marries
#     every other one and a cartesian product is presented as attribution. This
#     is the one that would look fine on every single-communicator capture.
#   * RANK SCOPE ON EVENT IDS. event_id is a per-PROCESS counter, so id 2 on
#     rank 0 and id 2 on rank 1 are different events. A parent join that drops
#     `rank` attributes one rank's proxy traffic to another rank's collective —
#     the same "a wire id is only unique with its scope" rule link_sql.hpp opens
#     with, arriving on a third kind of id.
#   * HANDLE AND DOMAIN SCOPE. algo/proto/func/datatype are intern handles.
#     Handle 1 is a different string on every rank and in every domain.
#   * ROWS THAT MUST SURVIVE. An execution row whose communicator cannot be
#     named is still a true statement about the algorithm, so it stays in the
#     table with NULL rank layout — and link_health counts it. An INNER JOIN
#     would delete exactly the rows a reader most needs to be told about.
#   * THE DOOR THAT DID NOT OPEN. Collectives recorded with no execution rows is
#     indistinguishable from a workload that ran none, unless something counts
#     it. That is link_health.nccl_profiler_door_shut, and this file checks it
#     fires — an assertion that cannot express the defect cannot test for it.
#
# Only the CSVs that carry the fixture: mount_raw_tables creates every other raw
# table empty with the right columns.
set -uo pipefail

PCOMM=${1:?usage: nccl_execution_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-nccl-exec-rule-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# Two ranks of one communicator (hash 111), plus a SPLIT communicator on rank 0
# whose hash the host-API door could not derive and recorded as 0.
cat > "$WORK/nccl_comm.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,comm,comm_hash,nccl_comm_hash,rank_,nranks,cuda_dev,pad
0,2,0,10,0,20,0,1,7001,111,0,2,0,0
1,2,0,10,0,20,0,1,7001,111,1,2,1,0
0,2,0,30,0,40,0,2,7002,0,0,2,0,0
CSV

# rank 0: two collectives on comm 111 (events 2 and 4), one on a communicator
#         whose hash 999 matches no nccl_comm row, and — event 8 — one whose own
#         hash is 0.
# rank 1: event ids 2 and 4 AGAIN — the same numbers, different events.
#
# EVENT 8 IS THE ONE THAT MAKES THE ZERO GUARD TESTABLE, and it was added after
# a mutation survived without it. Deleting `AND x.nccl_comm_hash <> 0` from the
# join changed nothing on the first version of this fixture, because no
# execution row there had hash 0 and the clause was dead code the test could not
# reach. It is reachable in the field: the plugin writes 0 when NCCL hands it a
# null context, and rank 0 has a nccl_comm row with hash 0 (the split), so
# without the guard those two 0s join and a collective is attributed to a
# communicator it has nothing to do with.
cat > "$WORK/nccl_coll_exec.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,func,algo,proto,datatype,nccl_comm_hash,event_id,parent_event_id,seq_number,count,root,n_channels,n_warps,pad
0,2,0,100,0,200,0,1,2,3,4,111,2,1,0,65536,0,2,17,0
0,2,0,300,0,400,0,5,6,3,4,111,4,3,1,1024,0,1,17,0
0,2,0,500,0,600,0,1,2,3,4,999,6,5,0,4096,0,1,17,0
0,2,0,700,0,800,0,1,2,3,4,0,8,7,0,2048,0,1,17,0
1,2,0,110,0,210,0,1,2,3,4,111,2,1,0,65536,0,2,17,0
1,2,0,310,0,410,0,5,6,3,4,111,4,3,1,1024,0,1,17,0
CSV

# rank 0 event 2 (AllReduce/RING) decomposes into a send and a recv to peer 1.
# rank 1 event 2 decomposes into ONE send to peer 0 — deliberately a different
# count from rank 0's, so a join that ignores `rank` produces the wrong number
# rather than the same number twice.
# The last row on rank 1 names parent 99, which no collective has: the
# unparented case, which must survive as a row with NULL algo.
cat > "$WORK/nccl_proxy_op.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,event_id,parent_event_id,peer,n_steps,chunk_size,pid,channel_id,is_send
0,2,0,120,0,180,0,10,2,1,8,524288,5000,0,1
0,2,0,120,0,180,0,11,2,1,4,524288,5000,1,0
1,2,0,130,0,190,0,10,2,0,8,524288,5001,0,1
1,2,0,140,0,195,0,12,99,0,2,1024,5001,0,1
CSV

# The clock, or every t0_ns is NULL and the temporal half of both tables is
# untested. One anchor per (rank, domain).
cat > "$WORK/clock_anchor.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,domain,ticks,wall_ns,ns_per_tick
0,1,0,0,0,0,0,0,0,1000000000,1.0
1,1,0,0,0,0,0,0,0,1000000000,1.0
CSV

# Handle 1 on rank 0 and handle 1 on rank 1 must be the SAME string here (both
# ranks ran the same collective), so the decoys carry the scope check instead:
# handle 2 in domain 3 is a kernel name, and rank 1 has a handle 5 that differs
# from rank 0's.
cat > "$WORK/intern_text.csv" <<'CSV'
rank,domain,handle,text
0,2,1,AllReduce
0,2,2,RING
0,2,3,SIMPLE
0,2,4,ncclFloat32
0,2,5,Broadcast
0,2,6,TREE
0,3,2,SHOULD_NOT_APPEAR_a_kernel_name
1,2,1,AllReduce
1,2,2,RING
1,2,3,SIMPLE
1,2,4,ncclFloat32
1,2,5,ReduceScatter
1,2,6,TREE
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

# --- nccl_execution ---------------------------------------------------------

# Six in, six out. Fewer means the LEFT JOIN became an INNER one and deleted the
# unresolvable rows; more means a join multiplied — which is exactly what the
# zero-hash guard prevents, since rank 0 has both an exec row and a comm row
# holding 0.
expect "every execution row survives, including the ones that cannot be named" \
    "SELECT count(*) FROM nccl_execution" "6"

# THE ZERO-HASH CLAUSE, and the reason this fixture has a split communicator at
# all. rank 0's comm 2 records nccl_comm_hash = 0. The collective on hash 999
# matches nothing, so its rank layout is NULL. If the join let 0 match 0 — or
# matched on anything other than equality of a non-zero hash — this row would
# come back attributed to a 2-rank communicator it has nothing to do with.
expect "an underivable communicator leaves the layout NULL, not guessed" \
    "SELECT count(*) FROM nccl_execution WHERE nccl_comm_hash = 999 AND nranks IS NULL" "1"

# THE GUARD ITSELF. The exec row with hash 0 and the nccl_comm row with hash 0
# are both on rank 0. Equality alone joins them; `<> 0` is what stops it. This
# assertion is the one a mutation of that clause has to fail, and on the first
# version of this fixture it did not exist.
expect "a zero hash on both sides does NOT join" \
    "SELECT count(*) FROM nccl_execution WHERE nccl_comm_hash = 0 AND nranks IS NOT NULL" "0"

expect "and that row is still present, unattributed" \
    "SELECT count(*) FROM nccl_execution WHERE nccl_comm_hash = 0" "1"

# The rows that DO resolve must carry the layout from their own rank's row.
expect "resolved rows carry the rank layout" \
    "SELECT string_agg(CAST(nccl_rank AS VARCHAR), '|' ORDER BY rank, t0_ns)
     FROM nccl_execution WHERE nranks = 2" "0|0|1|1"

# THE HANDLE JOIN. algo 2 is "RING" in InternDomain::String and a kernel name in
# InternDomain::KernelName; a join that forgets `domain` picks up the decoy.
expect "algo resolves through the string domain, not the kernel-name decoy" \
    "SELECT count(*) FROM nccl_execution WHERE algo = 'RING'" "4"

expect "the KernelName decoy at the same handle is not picked up" \
    "SELECT count(*) FROM nccl_execution WHERE algo LIKE 'SHOULD_NOT_APPEAR%'" "0"

# Handle 5 is Broadcast on rank 0 and ReduceScatter on rank 1. Dropping `rank`
# from the handle join resolves both to whichever the engine reached first.
expect "handle 5 means a different collective on each rank" \
    "SELECT string_agg(func, '|' ORDER BY rank) FROM nccl_execution WHERE proto = 'SIMPLE' AND n_channels = 1 AND nccl_comm_hash = 111" \
    "Broadcast|ReduceScatter"

# The temporal half. ns_per_tick is 1.0 and w0 is 1e9, so t0_ns is tick + 1e9.
expect "the interval lands on the wall clock" \
    "SELECT (t1_ns - t0_ns) FROM nccl_execution WHERE rank = 0 AND event_id = 2" "100"

# --- nccl_peer_traffic ------------------------------------------------------

expect "every proxy op survives, including the unparented one" \
    "SELECT count(*) FROM nccl_peer_traffic" "4"

# THE RANK-SCOPE CLAUSE, and the one that matters most. event_id 2 exists on
# BOTH ranks. rank 0's collective has two proxy ops, rank 1's has one. A parent
# join that drops `rank` gives every one of those ops both parents and the
# counts come back 4 and 2 instead of 2 and 1.
expect "proxy ops attach to their OWN rank's collective" \
    "SELECT string_agg(CAST(n AS VARCHAR), '|' ORDER BY rank) FROM (
       SELECT rank, count(*) AS n FROM nccl_peer_traffic WHERE algo IS NOT NULL GROUP BY rank)" \
    "2|1"

# planned_bytes is n_steps x chunk_size, computed rather than stored, and it is
# a SCHEDULE. 8 x 524288.
expect "planned_bytes is the product, not a stored number" \
    "SELECT planned_bytes FROM nccl_peer_traffic WHERE rank = 0 AND event_id = 10" "4194304"

expect "direction is decoded, both ways" \
    "SELECT string_agg(dir, '|' ORDER BY event_id) FROM nccl_peer_traffic WHERE rank = 0" \
    "send|recv"

# A proxy op naming a parent nobody recorded keeps its own facts and reports no
# collective — the same discipline as the unresolvable execution row above.
expect "an unparented proxy op survives with a NULL collective" \
    "SELECT count(*) FROM nccl_peer_traffic WHERE parent_event_id = 99 AND algo IS NULL AND peer = 0" "1"

# --- link_health ------------------------------------------------------------

# Each of these must be NON-ZERO on this fixture. A rubric row that cannot be
# made to fire is a row that will read as clean on the day it is wrong — the
# lesson from the wr_flight rows and from every_rank_says_what_it_was_asked_to_do.
# Two: the hash-999 row and the hash-0 row. Both are execution rows naming a
# communicator nccl_comm cannot supply, for the two different reasons.
expect "both unresolvable communicators are counted" \
    "SELECT n FROM link_health WHERE finding = 'nccl_exec_comm_unresolved'" "2"

expect "the unparented proxy op is counted" \
    "SELECT n FROM link_health WHERE finding = 'nccl_proxy_op_unparented'" "1"

# The door was open here — there ARE execution rows — so this one must be zero.
# Its firing case is checked below, on a second capture.
expect "the door-shut row is zero when the door was open" \
    "SELECT n FROM link_health WHERE finding = 'nccl_profiler_door_shut'" "0"

# --- the door that did not open ---------------------------------------------
#
# A SECOND capture: collectives at the host-API door, nothing from the plugin.
# This is what a run with NCCL_PROFILER_PLUGIN unset produces, and the whole
# point is that it is indistinguishable from a healthy capture unless something
# counts it.
SHUT="$WORK/shut"
mkdir -p "$SHUT"
cp "$WORK/nccl_comm.csv" "$WORK/clock_anchor.csv" "$WORK/intern_text.csv" "$SHUT/"
cat > "$SHUT/nccl_collective.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,comm,sendbuff,recvbuff,stream,count,root,op,datatype,redop,pad
0,2,0,100,0,200,0,1,10,11,12,65536,-1,0,7,0,0
0,2,0,300,0,400,0,1,10,11,12,65536,-1,0,7,0,0
1,2,0,110,0,210,0,1,10,11,12,65536,-1,0,7,0,0
CSV

"$PCOMM" link "$SHUT" > "$SHUT/link.out" 2>&1
if [ $? -ne 0 ] || grep -q "SOME FAILED" "$SHUT/link.out"; then
    cat "$SHUT/link.out"
    echo "FAIL: link did not complete on the door-shut capture"
    exit 1
fi

got=$("$PCOMM" sql "$SHUT" \
      "SELECT n FROM link_health WHERE finding = 'nccl_profiler_door_shut'" 2>/dev/null | tail -1)
if [ "$got" != "3" ]; then
    echo "FAIL: the door-shut row did not fire on a capture with no execution rows"
    echo "  expected: 3   got: ${got:-<no result>}"
    echo "  This is the check that keeps a silently-degraded NCCL capture from"
    echo "  reading exactly like a healthy one."
    fail=1
fi

# And it must NOT fire on a capture with no NCCL at all, or every ibverbs-only
# run in the suite grows a permanent defect row.
NONE="$WORK/none"
mkdir -p "$NONE"
cp "$WORK/clock_anchor.csv" "$NONE/"
"$PCOMM" link "$NONE" > "$NONE/link.out" 2>&1
got=$("$PCOMM" sql "$NONE" \
      "SELECT n FROM link_health WHERE finding = 'nccl_profiler_door_shut'" 2>/dev/null | tail -1)
if [ "$got" != "0" ]; then
    echo "FAIL: the door-shut row fired on a capture that has no NCCL in it"
    echo "  expected: 0   got: ${got:-<no result>}"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "PASS: nccl_execution and nccl_peer_traffic join on rank, hash and parent"
exit "$fail"
