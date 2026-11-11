#!/usr/bin/env bash
# The transport-attribution rule, against a capture built to break it.
#
# WHY THIS EXISTS. `transport_mix` answers the one question comm_edges cannot:
# a UCX rank matrix says rank 0 sent 408 MB to rank 1 and has no idea whether
# that crossed a NIC, a shared-memory segment or a CUDA IPC handle. Getting the
# answer needs TWO joins that had never existed in this layer before — an intern
# handle to the string it stands for, and a uct_send to the interface that
# carried it — and each has a way to be plausibly wrong.
#
# It also covers the branch NO HARDWARE CAPTURE HAS PRODUCED. MN5 job 45113099
# ran 295 uct_send rows and every one was am_short/am_bcopy/am_zcopy: no Get*,
# no rendezvous, no hardware tag matching. The three-family split in
# _uct_sends — out vs in vs advertised — is therefore asserted here or nowhere,
# and it is the split that stops a rendezvous advertisement being counted as a
# transfer and then counted a second time when the peer pulls it.
#
# THE FIXTURE HAS TWO RANKS AND REUSES ORDINALS ON PURPOSE. iface pointer 10
# exists on both ranks and means a DIFFERENT transport on each; intern handle 1
# is "rc_mlx5" on rank 0 and "sysv" on rank 1. Every join here is keyed on a
# PAIR — (rank, iface), (rank, domain, handle) — and a fixture whose second key
# component is unique cannot tell a correct join from one that dropped it. Same
# defect comm_edges_rule.sh was written for, on two different ids.
set -uo pipefail

COMSHU=${1:?usage: transport_mix_rule.sh <comshu-binary>}
if [ ! -x "$COMSHU" ]; then echo "SKIP: no comshu binary at $COMSHU"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-transport-mix-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The dictionary, as the export now writes it. domain 2 is InternDomain::String
# (Pc, Pointer, String, KernelName, TypeName) — the same numeric-enum rule the
# rest of this layer follows, because these tables are the RAW streams.
#
# EVERY ROW IS A CLAIM:
#   rank 0 handle 1 "rc_mlx5"   rank 1 handle 1 "sysv"    same handle, two ranks
#   rank 0 handle 3 "cuda_ipc"                            intra-node, not shm
#   rank 0 handle 9 in domain 3 (KernelName) is "sysv"    a name in the WRONG
#                                                         domain, which must not
#                                                         resolve a tl_name
#   rank 1 handle 4 has TWO texts                         ambiguous: resolves to
#                                                         nothing, by design
cat > "$WORK/intern_text.csv" <<'CSV'
rank,domain,handle,text
0,2,1,rc_mlx5
0,2,2,mlx5_0:1
0,2,3,cuda_ipc
0,2,4,mlx5_1:1
0,3,9,sysv
1,2,1,sysv
1,2,2,memory
1,2,4,rc_mlx5
1,2,4,ud_mlx5
1,2,5,mlx5_0:1
CSV

# The interfaces. iface 10 is rc_mlx5 on rank 0 and sysv on rank 1: a join on
# `iface` alone attributes rank 1's shared-memory traffic to the fabric, and
# link_health's intra-node count silently reads 0.
#
#   rank 0 iface 10 -> rc_mlx5  on mlx5_0:1     network
#   rank 0 iface 11 -> cuda_ipc (no dev name)   intra-node
#   rank 0 iface 12 -> handle 9, which is a KernelName, not a String -> unknown
#   rank 1 iface 10 -> sysv     on memory       intra-node
#   rank 1 iface 13 -> handle 4, claimed by two strings -> unknown
cat > "$WORK/uct_iface.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,open_mode,iface,worker,md,tl_name,dev_name,instrumented,patched_ops,pad0,pad1
0,1,0,100,0,0,0,1,10,50,60,1,2,1,13,0,0
0,1,0,100,0,0,0,1,11,50,61,3,0,1,6,0,0
0,1,0,100,0,0,0,1,12,50,62,9,0,1,5,0,0
1,1,0,100,0,0,0,1,10,51,63,1,2,1,6,0,0
1,1,0,100,0,0,0,1,13,51,64,4,5,1,13,0,0
CSV

# The operations. op values are UctOp, pinned in ucx_deep_records.hpp:
#   0 am_short  2 am_bcopy  3 am_zcopy  6 put_zcopy  9 get_zcopy  13 tag_rndv_zcopy
#
# EVERY ROW IS A CLAIM ABOUT A SUM:
#   rank 0 iface 10  am_short   100 payload,  8 header   -> out 100, header 8
#   rank 0 iface 10  am_short   200 payload,  8 header   -> out 300, header 16
#   rank 0 iface 10  am_bcopy   status -6 (NO_RESOURCE)  -> EXCLUDED entirely
#   rank 0 iface 10  put_zcopy  4096                     -> out
#   rank 0 iface 10  get_zcopy  8192                     -> IN, never out
#   rank 0 iface 10  tag_rndv   1048576 payload, 40 hdr  -> ADVERTISED, and only
#                                                           the 40 went out
#   rank 0 iface 11  am_bcopy   64                       -> cuda_ipc, intra-node
#   rank 0 iface 12  am_short   7                        -> unknown scope
#   rank 1 iface 10  am_bcopy   500 x2                   -> sysv, intra-node
#   rank 1 iface 13  am_short   3                        -> unknown scope
#   rank 1 iface 99  am_short   9                        -> NO uct_iface AT ALL
#
# The rejected row is 999999 payload so that counting it is loud rather than
# subtle, and it is UCS_ERR_NO_RESOURCE specifically: that is UCX's "retry
# later", it fires in proportion to how busy a transport is, and counting it
# would inflate exactly the transports a reader is most interested in.
cat > "$WORK/uct_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,payload,header,remote_addr,tag,ep,iface,status,op,iovcnt,pad0,pad1
0,2,0,1000,0,1010,0,100,8,0,0,20,10,0,0,0,0,0
0,2,0,1020,0,1030,0,200,8,0,0,20,10,0,0,0,0,0
0,2,0,1040,0,1050,0,999999,0,0,0,20,10,-6,2,0,0,0
0,2,0,1060,0,1070,0,4096,0,70000,0,20,10,0,6,1,0,0
0,2,0,1080,0,1090,0,8192,0,70000,0,20,10,0,9,1,0,0
0,2,0,1100,0,1110,0,1048576,40,0,4919,20,10,1,13,2,0,0
0,2,0,1120,0,1130,0,64,0,0,0,21,11,0,2,0,0,0
0,2,0,1140,0,1150,0,7,0,0,0,22,12,0,0,0,0,0
1,2,0,1160,0,1170,0,500,0,0,0,23,10,0,2,0,0,0
1,2,0,1180,0,1190,0,500,0,0,0,23,10,0,2,0,0,0
1,2,0,1200,0,1210,0,3,0,0,0,24,13,0,0,0,0,0
1,2,0,1220,0,1230,0,9,0,0,0,25,99,0,0,0,0,0
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
        echo "FAIL: $1"
        echo "  expected: $3"
        echo "  got:      ${got:-<no result>}"
        echo "  query:    $2"
        fail=1
    fi
    return 0
}

# The whole profile in one value, for the reason comm_edges_rule.sh states its
# matrix whole: every defect this file is about changes it. Dropping `rank` from
# the intern join renames rank 1's sysv to rc_mlx5; dropping it from the iface
# join moves rank 1's traffic onto the fabric; counting the rejected row adds
# 999999; folding the rendezvous advertisement into out_bytes adds 1048576.
expect "the profile" \
    "SELECT string_agg(rank||':'||coalesce(tl_name,'-')||'/'||coalesce(dev_name,'-')||':'||scope||':'||op||':'||ops||':'||out_bytes||':'||in_bytes||':'||advertised_bytes||':'||header_bytes, ' ' ORDER BY rank, scope, tl_name, dev_name, op) FROM transport_mix" \
    "0:cuda_ipc/-:intra-node:am_bcopy:1:64:0:0:0 0:rc_mlx5/mlx5_0:1:network:am_short:2:300:0:0:16 0:rc_mlx5/mlx5_0:1:network:get_zcopy:1:0:8192:0:0 0:rc_mlx5/mlx5_0:1:network:put_zcopy:1:4096:0:0:0 0:rc_mlx5/mlx5_0:1:network:tag_rndv_zcopy:1:0:0:1048576:40 0:-/-:unknown:am_short:1:7:0:0:0 1:sysv/memory:intra-node:am_bcopy:2:1000:0:0:0 1:-/mlx5_0:1:unknown:am_short:1:3:0:0:0 1:-/-:unknown:am_short:1:9:0:0:0"

# --- the intern join, one clause at a time ----------------------------------
#
# Handle 1 is "rc_mlx5" on rank 0 and "sysv" on rank 1. This is the whole reason
# `rank` is in the key, and a join that dropped it would report a plausible,
# confident, wrong transport for half the job.
expect "an intern handle is only unique with its rank" \
    "SELECT string_agg(DISTINCT rank||'='||tl_name, ',' ORDER BY rank||'='||tl_name) FROM transport_mix WHERE tl_name IN ('rc_mlx5','sysv')" \
    "0=rc_mlx5,1=sysv"

# Handle 9 on rank 0 is "sysv" in the KernelName domain. A tl_name is a String,
# and a join that ignored `domain` would name iface 12 "sysv" and file its
# traffic as intra-node — a wrong answer that looks entirely reasonable.
expect "a name in the wrong domain does not resolve a transport" \
    "SELECT scope FROM transport_mix WHERE rank = 0 AND ops = 1 AND out_bytes = 7" \
    "unknown"

# Rank 1 handle 4 is claimed by two strings. _intern_name refuses it rather than
# taking min(text), which would silently pick 'rc_mlx5' over 'ud_mlx5'.
expect "an ambiguous handle resolves to no name at all" \
    "SELECT count(*) FROM transport_mix WHERE tl_name IS NOT NULL AND rank = 1 AND scope = 'unknown'" \
    "0"

# THE TWO NAMES RESOLVE INDEPENDENTLY, and this clause exists because the first
# draft of this test asserted otherwise. Rank 1's iface 13 has an ambiguous
# tl_name (handle 4, two texts) and a perfectly good dev_name (handle 5,
# "mlx5_0:1"); it must keep the half that resolved rather than collapse to the
# same all-NULL row as iface 99, which has no uct_iface record at all. Those are
# two different states of knowledge — "we know the device and not the transport"
# is not "we know nothing" — and a reader chasing an unnamed transport can start
# from the device when the capture has one.
expect "a half-resolved interface keeps the half that resolved" \
    "SELECT string_agg(coalesce(dev_name,'-')||'='||ops, ',' ORDER BY dev_name) FROM transport_mix WHERE rank = 1 AND scope = 'unknown'" \
    "mlx5_0:1=1,-=1"

# --- the three families, which no hardware capture has yet exercised ---------
#
# A rendezvous advertisement is not a transfer. Only the 40-byte header left
# this node; the megabyte is what the peer was INVITED to pull, and if the peer
# pulls it the peer's own Get* records it. Folding it into out_bytes bills the
# same buffer twice, in opposite directions.
expect "a rendezvous advertisement is not outbound bytes" \
    "SELECT out_bytes||'/'||advertised_bytes||'/'||header_bytes FROM transport_mix WHERE op = 'tag_rndv_zcopy'" \
    "0/1048576/40"

# A get PULLS. Counting it as outbound reverses the direction of every
# rendezvous completion in a capture.
expect "a get is inbound, not outbound" \
    "SELECT out_bytes||'/'||in_bytes FROM transport_mix WHERE op = 'get_zcopy'" \
    "0/8192"

# UCS_ERR_NO_RESOURCE moved nothing. 999999 is chosen so leaking it is
# unmissable, and am_bcopy on iface 10 of rank 0 has no other row — so if the
# filter goes, this op appears at all.
expect "a retried operation is not counted" \
    "SELECT count(*) FROM transport_mix WHERE rank = 0 AND op = 'am_bcopy' AND tl_name = 'rc_mlx5'" \
    "0"

# --- the health rows --------------------------------------------------------
#
# Three unnameable operations: iface 12 (wrong domain), iface 13 (ambiguous
# handle), iface 99 (no uct_iface row at all). Three different causes, one
# count, because they mean the same thing to a consumer — this traffic has no
# transport.
expect "operations with no nameable transport are counted" \
    "SELECT n FROM link_health WHERE finding = 'unnamed_uct_transport'" "3"

# cuda_ipc x1 + sysv x2 — three operations that never touched a network.
#
# This used to assert link_health.ucx_edges_include_intranode = 3, a global
# count standing in for a caveat comm_edges could not state about itself. That
# row is gone: comm_edges carries `scope` per edge now, so the caveat is a
# column rather than a footnote, and a rubric row that fired on every healthy
# multi-rank-per-node capture was reporting the normal case as a defect.
#
# comm_edges.scope is NOT asserted here, and the reason is worth stating so the
# next reader does not "fix" it back. This fixture holds two raw tables —
# uct_iface and uct_send — because it exists to test the UCT layer. comm_edges'
# ucx lane is built from the UCP tables, which are absent, so comm_edges is
# EMPTY here and any claim about its scope column would pass vacuously. That is
# the exact defect tests/hpc/suite/README.md warns about, and it was live for
# about ten minutes: the first version of this hunk asserted
# string_agg(DISTINCT scope) over an empty table and failed with <no result>,
# which at least failed loudly rather than passing.
#
# The real check lives where the data does:
# tests/hpc/suite/asserts/comm_edges_scope_agrees_with_topology.sql, which runs
# against every case in the suite.
#
# The deleted row must stay deleted, and THAT is testable here, because
# link_health is derived from _uct_scoped and this fixture does populate it. A
# rubric that quietly regrows a finding firing on every healthy capture is how
# link_health becomes noise.
expect "the obsolete intra-node caveat is not in the rubric" \
    "SELECT count(*) FROM link_health WHERE finding = 'ucx_edges_include_intranode'" \
    "0"

# 'unknown' is never folded into either real bucket. A transport this build does
# not know the name of must stay visible as unknown rather than be guessed into
# 'network' — which is the guess that would make the intra-node caveat read 0 on
# a stack using a transport added after this list was written.
expect "unknown is its own scope and absorbs nothing" \
    "SELECT string_agg(DISTINCT scope, ',' ORDER BY scope) FROM transport_mix" \
    "intra-node,network,unknown"

# THE ORACLE THE OTHER TABLE RESTS ON. transport_mix is deliberately NOT a lane
# of comm_edges — one UCP send becomes several UCT operations, so unioning them
# would bill the same bytes at two layers. The cheapest way to keep that
# decision from being quietly reversed is to assert it: this capture has UCT
# traffic and no UCP records at all, and comm_edges must still be empty.
expect "UCT traffic does not become comm_edges rows" \
    "SELECT count(*) FROM comm_edges" "0"

[ "$fail" -eq 0 ] || exit 1
echo "ok: transport attribution holds on a capture built to break it"
