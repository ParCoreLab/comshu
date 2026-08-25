#!/usr/bin/env bash
# The comm matrix rule, against a capture built to break it.
#
# WHY THIS EXISTS. comm_edges is the table behind the headline panel and it had
# no local test at all: every assertion over it lived in tests/hpc, which needs
# a GPU cluster, a queue slot and a capture. So the rule that turns sends into
# rank-space edges could only be regression-tested by booking MN5 — and a red
# board there has more than once been the harness rather than the tracer.
#
# It also covers a branch NO REAL CAPTURE HAS EVER EXERCISED. _nvlink_sends
# derives edge direction from the memory op, because a Store pushes to the peer
# and a Load pulls from it. link_sql.hpp says of that branch: "Only Store
# appears in the corpus today, which is precisely why the Load case is written
# now rather than discovered by whichever capture first has one." This is where
# it gets discovered. Atomic counts as a write and Prefetch as a read, and both
# are here for the same reason.
#
# THE FIXTURE IS TWO NODES ON PURPOSE, and the ordinals collide on both axes:
# qp_num 5 exists on both HCAs and device 1 exists on both nodes. Every join in
# this layer is keyed on a PAIR — (lid, qp_num), (node, device) — and a fixture
# where the second half of each key is unique cannot tell a correct join from
# one that dropped it. That is the same defect flow_pairing_agree.sh could not
# see and wr_flow_rule.sh was written for, in a different join.
#
# Three CSVs would not be enough here: mount_raw_tables creates every other raw
# table empty with the right columns, so the derived layer builds on a capture
# containing only what this test is about.
set -uo pipefail

PCOMM=${1:?usage: comm_edges_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-comm-edges-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The topology, and it is the whole point of the fixture:
#
#   node 111  rank 0 (dev 0, lid 10)   rank 1 (dev 1, lid 10)
#   node 222  rank 2 (dev 0, lid 20)   rank 3 (dev 1, lid 20)
#   nowhere   rank 4 (dev 0, no lid, no node_identity)
#
# device 1 is TWO different GPUs and qp_num 5 is TWO different queue pairs.
# Rank 4 exists to be unresolvable: it has cross-device traffic and no way to
# name a node, which is the pure-NVLink capture's shape.
cat > "$WORK/node_identity.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,node_hash
0,1,0,100,0,0,0,111
1,1,0,100,0,0,0,111
2,1,0,100,0,0,0,222
3,1,0,100,0,0,0,222
CSV

# _device_owner is fed from mem_object AND gpu_access.run_device. Rank 3 has no
# GPU traffic at all, so without its mem_object row nothing would name its GPU
# and rank 2's stores to device 1 would resolve to nobody — which is exactly the
# nvshmem_multinode case the view's comment records.
cat > "$WORK/mem_object.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,id,name,range_base,range_size,device
0,2,0,100,0,900,0,1,0,4096,1024,0
1,2,0,100,0,900,0,1,0,4096,1024,1
2,2,0,100,0,900,0,1,0,4096,1024,0
3,2,0,100,0,900,0,1,0,4096,1024,1
CSV

# state 2 is RTR — the only transition that carries the remote endpoint.
#
# qp_num 5 appears on BOTH HCAs, connected to each other. Keyed on (lid, qp_num)
# these are two queue pairs and rank 0's traffic lands on rank 2. Keyed on
# qp_num alone they are one, _owner reports two claimants, and rank 0's sends
# resolve to min(rank) = rank 0 — an edge from a rank to itself, which is what
# link_health's colliding_endpoints row exists to name.
cat > "$WORK/ib_modify_qp.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,qp,qp_num,dest_qp_num,dest_lid,src_lid,state
0,1,0,200,0,0,0,1,5,5,20,10,2
1,1,0,200,0,0,0,2,6,7,20,10,2
2,1,0,200,0,0,0,3,5,5,10,20,2
3,1,0,200,0,0,0,4,7,6,10,20,2
CSV

# rank 3's qp_num 9 never transitioned to RTR, so nothing names its peer. Five
# sends that must be COUNTED as unresolved rather than dropped into silence or
# attached to a plausible neighbour.
cat > "$WORK/ib_post_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,qp,wr_id,remote_addr,rkey,length,qp_num,send_flags,opcode,num_sge
0,2,0,1000,0,1010,0,1,1,4096,7,100,5,2,0,1
0,2,0,1100,0,1110,0,1,2,4196,7,100,5,2,0,1
0,2,0,1200,0,1210,0,1,3,4296,7,100,5,2,0,1
1,2,0,1300,0,1310,0,2,4,4096,7,50,6,2,0,1
1,2,0,1400,0,1410,0,2,5,4146,7,50,6,2,0,1
2,2,0,1500,0,1510,0,3,6,4096,7,64,5,2,0,1
3,2,0,1600,0,1610,0,9,7,4096,7,8,9,2,0,1
3,2,0,1700,0,1710,0,9,8,4096,7,8,9,2,0,1
3,2,0,1800,0,1810,0,9,9,4096,7,8,9,2,0,1
3,2,0,1900,0,1910,0,9,10,4096,7,8,9,2,0,1
3,2,0,2000,0,2010,0,9,11,4096,7,8,9,2,0,1
CSV

# The DEVX plane, carrying its own connect record and its own sends. Both are
# unioned into _sends and _owner beside the verbs half, which is what lets a
# consumer of comm_edges never learn which plane a run used — while `transport`
# survives so it can still be asked.
cat > "$WORK/mlx5_devx_qp_connect.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,dest_gid_prefix,dest_gid_iface,qp_num,dest_qp_num,dest_lid,src_lid,port_num
1,1,0,300,0,0,0,0,0,30,31,20,10,1
3,1,0,300,0,0,0,0,0,31,30,10,20,1
CSV

cat > "$WORK/mlx5_device_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,laddr,raddr,qp_num,wqe_idx,lkey,rkey,length,opcode
1,2,0,2100,0,2110,0,8192,4096,30,0,3,7,200,0
1,2,0,2200,0,2210,0,8192,4096,30,1,3,7,200,0
CSV

# op: 0 = Load, 1 = Store, 2 = Atomic, 3 = Prefetch. space 0 = Global.
#
# EVERY ROW HERE IS A DIRECTION CLAIM.
#   rank 0, dev 0 -> mem on dev 1, Store x2 @ 8   push, so 0 -> 1
#   rank 0, dev 0 -> mem on dev 1, Load  x3 @ 4   PULL, so 1 -> 0   <-- untested branch
#   rank 1, dev 1 -> mem on dev 0, Atomic  @ 16   counts as a write, 1 -> 0
#   rank 1, dev 1 -> mem on dev 0, Prefetch @ 32  counts as a read,  0 -> 1
#   rank 2, dev 0 -> mem on dev 1, Store   @ 64   node 222, so 2 -> 3 and NOT 2 -> 1
#   rank 0, dev 0 -> mem on dev 0, Store   @ 999  LOCAL: not an edge at all
#   rank 4, dev 0 -> mem on dev 1, Store x2 @ 7   no node: dst is -1, and counted
#
# Treating Load as a write collapses the two 0<->1 edges into one; scoping
# _device_owner by device alone sends rank 2's store to rank 1 on the other node.
cat > "$WORK/gpu_access.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,pc,object,address,launch,size,op,space,run_device,mem_device,thread
0,3,0,3000,0,3100,0,1,1,4096,0,8,1,0,0,1,0
0,3,0,3000,0,3100,0,1,1,4104,0,8,1,0,0,1,0
0,3,0,3000,0,3100,0,1,1,4112,0,4,0,0,0,1,0
0,3,0,3000,0,3100,0,1,1,4116,0,4,0,0,0,1,0
0,3,0,3000,0,3100,0,1,1,4120,0,4,0,0,0,1,0
1,3,0,3000,0,3100,0,1,1,4096,0,16,2,0,1,0,0
1,3,0,3000,0,3100,0,1,1,4112,0,32,3,0,1,0,0
2,3,0,3000,0,3100,0,1,1,4096,0,64,1,0,0,1,0
0,3,0,3000,0,3100,0,1,1,4096,0,999,1,0,0,0,0
4,3,0,3000,0,3100,0,1,1,4096,0,7,1,0,0,1,0
4,3,0,3000,0,3100,0,1,1,4104,0,7,1,0,0,1,0
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
    return 0
}

# The whole matrix in one value. Every defect this file is about changes it:
# a qp_num-only join moves the verbs edges, a device-only join moves 2>3 to 2>1,
# treating Load as a write merges 0>1 and 1>0, and dropping the local-access
# filter adds 999 bytes to some edge.
expect "the matrix" \
    "SELECT string_agg(src_rank||'>'||dst_rank||':'||transport||':'||sends||':'||bytes, ' ' ORDER BY transport, src_rank, dst_rank) FROM comm_edges" \
    "1>3:devx:2:400 0>1:nvlink:3:48 1>0:nvlink:4:28 2>3:nvlink:1:64 4>-1:nvlink:2:14 0>2:verbs:3:300 1>3:verbs:2:100 2>0:verbs:1:64 3>-1:verbs:5:40"

# Stated again as the invariant rather than as a value, because it is the clause
# no capture in the corpus can exercise: a Load moves bytes FROM the peer, so the
# peer is the source. If this reads 0 the direction rule has collapsed.
expect "a Load makes the peer the source, not the actor" \
    "SELECT sends FROM comm_edges WHERE src_rank = 1 AND dst_rank = 0 AND transport = 'nvlink'" \
    "4"

# qp_num 5 is two queue pairs on two HCAs. One claimant each, or the pair key
# has been dropped somewhere.
expect "no (lid, qp_num) is claimed twice" \
    "SELECT n FROM link_health WHERE finding = 'colliding_endpoints'" "0"

# Rank 2 and rank 1 both own a 'device 1'. The edge must follow the node.
expect "device ordinals are scoped by node" \
    "SELECT dst_rank FROM comm_edges WHERE src_rank = 2 AND transport = 'nvlink'" "3"

# Five sends on a queue pair that never reached RTR. Counted, not hidden.
expect "sends with no resolvable peer are counted" \
    "SELECT n FROM link_health WHERE finding = 'unresolved_sends'" "5"

# Rank 4 has traffic and no node. The hole is reported rather than guessed at.
expect "NVLink with no node identity is counted" \
    "SELECT n FROM link_health WHERE finding = 'unresolved_nvlink'" "2"

# A local access is not communication. It must not appear as an edge at all —
# and 999 is chosen so that if it leaks in, it leaks in visibly.
expect "a local access is not an edge" \
    "SELECT count(*) FROM comm_edges WHERE bytes >= 999" "0"

# The two planes stay distinguishable even though the matrix does not branch.
expect "both transports survive into the table" \
    "SELECT string_agg(DISTINCT transport, ',' ORDER BY transport) FROM comm_edges" \
    "devx,nvlink,verbs"

# NVLink is counted in ACCESSES and verbs in MESSAGES. Summing them would be a
# category error, and the unit column is what stops a panel doing it.
expect "the unit says which is which" \
    "SELECT string_agg(DISTINCT transport||'='||unit, ',' ORDER BY transport||'='||unit) FROM comm_edges" \
    "devx=messages,nvlink=accesses,verbs=messages"

[ "$fail" -eq 0 ] || exit 1
echo "ok: comm matrix holds on a capture built to break it"
