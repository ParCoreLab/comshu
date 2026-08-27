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

# --- the UCX lane -----------------------------------------------------------
#
# UCX resolves a peer WITHOUT a queue pair: this rank's endpoint remembers the
# prefix hash of the address it dialled, and the rank owning that address wrote
# the same hash for its own worker. That is the only join in the UCX records
# crossing a rank, and until 2026-08-27 comm_edges had no lane for it at all —
# so an MPI-over-UCX capture holding tens of GB produced ZERO rows (see
# tests/hpc/apps/, three studies).
#
#   rank 0  worker 100  addr 1000  prefix 10
#   rank 1  worker 101  addr 1001  prefix 11
#   rank 2  worker 102  addr 1002  prefix 99   <-- collides with rank 3
#   rank 3  worker 103  addr 1003  prefix 99   <-- same prefix, different addr
#
# The collision is the point. UcxWorkerAddress records two hashes precisely so a
# prefix clash is VISIBLE rather than a confident wrong join; a lane that took
# min(rank) would attribute rank 0's traffic on endpoint 202 to rank 2 and look
# entirely plausible doing it.
cat > "$WORK/ucx_worker_address.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,worker,addr_hash,prefix_hash,addr_len
0,1,0,100,0,0,0,100,1000,10,64
1,1,0,100,0,0,0,101,1001,11,64
2,1,0,100,0,0,0,102,1002,99,64
3,1,0,100,0,0,0,103,1003,99,64
CSV

#   ep 200 on rank 0 -> prefix 11 -> rank 1     resolves
#   ep 201 on rank 1 -> prefix 10 -> rank 0     resolves
#   ep 202 on rank 0 -> prefix 99 -> AMBIGUOUS  must fall to -1, not to rank 2
#   ep 203 on rank 2 -> prefix 10 -> rank 0     resolves, and carries the get
cat > "$WORK/ucx_ep_connection.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,remote_prefix_hash
0,1,0,200,0,0,0,200,11
1,1,0,200,0,0,0,201,10
0,1,0,200,0,0,0,202,99
2,1,0,200,0,0,0,203,10
CSV

# status_class: 0 Immediate, 1 Error, 2 Request. door is not read by this lane.
#
# EVERY ROW IS A CLAIM ABOUT THE PREDICATES.
#   rank 0 ep 200  two sends, 100 + 200 units          -> 0>1, 2 sends, 300
#   rank 0 ep 200  one send with status_class = 1      -> EXCLUDED (rejected)
#   rank 1 ep 201  one send, 50 units                  -> 1>0, 1 send, 50
#   rank 0 ep 202  one send, 7 units                   -> 0>-1, ambiguous prefix
#
# The error row is 9999 units so that counting it is loud rather than subtle.
cat > "$WORK/ucx_tag_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,buffer,request,count,tag,status,status_class,door
0,2,0,1000,0,1010,0,200,1,0,100,44,0,0,0
0,2,0,1020,0,1030,0,200,1,0,200,44,0,2,0
0,2,0,1040,0,1050,0,200,1,0,9999,44,-3,1,0
1,2,0,1060,0,1070,0,201,1,0,50,44,0,0,0
0,2,0,1080,0,1090,0,202,1,0,7,44,0,0,0
CSV

# An active message adds to the same edge: payload only, since UcxAmSend keeps
# header_length separate on purpose and a header is control traffic.
cat > "$WORK/ucx_am_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,buffer,request,am_id,count,header_length,status,status_class
0,2,0,1100,0,1110,0,200,1,0,7,25,999,0,0
CSV

# op: 0 Put, 1 Get.
#   rank 1 ep 201 PUT 30  -> outbound, joins 1>0
#   rank 2 ep 203 GET 80  -> INBOUND: rank 2 issues it, the bytes come FROM
#                            rank 0, so the edge is 0>2 and not 2>0
# A lane that ignores op counts the get as outbound and reverses that edge.
cat > "$WORK/ucx_rma.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,buffer,request,rkey,count,remote_addr,status,status_class,op,door
1,2,0,1200,0,1210,0,201,1,0,9,30,4096,0,0,0,0
2,2,0,1300,0,1310,0,203,1,0,9,80,4096,0,0,1,0
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
    "1>3:devx:2:400 0>1:nvlink:3:48 1>0:nvlink:4:28 2>3:nvlink:1:64 4>-1:nvlink:2:14 0>-1:ucx:1:7 0>1:ucx:3:325 0>2:ucx:1:80 1>0:ucx:2:80 0>2:verbs:3:300 1>3:verbs:2:100 2>0:verbs:1:64 3>-1:verbs:5:40"

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

# The one UCX send on the ambiguous prefix. Without this row a capture whose
# ENTIRE matrix failed to resolve reports a clean bill of health — the same
# failure unresolved_nvlink was added for, arriving a third time on a third key.
expect "UCX with an unresolvable prefix is counted" \
    "SELECT n FROM link_health WHERE finding = 'unresolved_ucx'" "1"

# A local access is not communication. It must not appear as an edge at all —
# and 999 is chosen so that if it leaks in, it leaks in visibly.
expect "a local access is not an edge" \
    "SELECT count(*) FROM comm_edges WHERE bytes >= 999" "0"

# The planes stay distinguishable even though the matrix does not branch.
expect "every transport survives into the table" \
    "SELECT string_agg(DISTINCT transport, ',' ORDER BY transport) FROM comm_edges" \
    "devx,nvlink,ucx,verbs"

# NVLink is counted in ACCESSES, verbs in MESSAGES, UCX in DATATYPE UNITS.
# Summing them would be a category error, and the unit column is what stops a
# panel doing it. UcxTagSend.count is a UCP datatype count and equals bytes only
# for a contiguous 8-bit datatype, which the capture does not record.
expect "the unit says which is which" \
    "SELECT string_agg(DISTINCT transport||'='||unit, ',' ORDER BY transport||'='||unit) FROM comm_edges" \
    "devx=messages,nvlink=accesses,ucx=datatype-units,verbs=messages"

# --- the UCX lane, one clause at a time -------------------------------------
#
# The whole UCX matrix, for the same reason the verbs one is stated whole: every
# defect in this lane changes it. Losing the prefix join sends everything to -1;
# ignoring the ambiguity check attributes 0>-1 to rank 2; ignoring op reverses
# the get; counting the error row inflates 0>1 by 9999.
expect "the ucx matrix" \
    "SELECT string_agg(src_rank||'>'||dst_rank||':'||sends||':'||bytes, ' ' ORDER BY src_rank, dst_rank) FROM comm_edges WHERE transport = 'ucx'" \
    "0>-1:1:7 0>1:3:325 0>2:1:80 1>0:2:80"

# A prefix claimed by two workers resolves to NEITHER. UcxWorkerAddress carries
# addr_hash beside prefix_hash so this is detectable at all; taking min(rank)
# here would produce a confident, wrong, entirely plausible edge.
expect "an ambiguous prefix does not resolve to a rank" \
    "SELECT sends FROM comm_edges WHERE transport = 'ucx' AND src_rank = 0 AND dst_rank = -1" \
    "1"

# A get PULLS: rank 2 issues it, the bytes travel from rank 0. Reading it as
# outbound would give 2>0 and reverse a real dependency in the graph.
expect "a get makes the peer the source, not the actor" \
    "SELECT bytes FROM comm_edges WHERE transport = 'ucx' AND src_rank = 0 AND dst_rank = 2" \
    "80"

# The rejected send moved nothing. 9999 is chosen so leaking it is unmissable.
expect "a rejected send is not counted" \
    "SELECT count(*) FROM comm_edges WHERE transport = 'ucx' AND bytes >= 9999" "0"

# tag_send + am_send land on one edge: 100 + 200 (tag) + 25 (am payload) = 325,
# and the am header of 999 is control traffic that must not be added.
expect "am payload joins the edge and its header does not" \
    "SELECT bytes FROM comm_edges WHERE transport = 'ucx' AND src_rank = 0 AND dst_rank = 1" \
    "325"

# THE ORACLE THE WHOLE TABLE RESTS ON. link_sql.hpp promises that the rows where
# transport IN ('verbs','devx') are byte-identical to CsvAgg's comm_edges. A new
# lane must not disturb that subset, and the cheapest way to keep the promise
# executable is to restate it here whenever a lane is added.
expect "the verbs/devx subset is untouched by the new lane" \
    "SELECT string_agg(src_rank||'>'||dst_rank||':'||sends||':'||bytes, ' ' ORDER BY transport, src_rank, dst_rank) FROM comm_edges WHERE transport IN ('verbs','devx')" \
    "1>3:2:400 0>2:3:300 1>3:2:100 2>0:1:64 3>-1:5:40"

[ "$fail" -eq 0 ] || exit 1
echo "ok: comm matrix holds on a capture built to break it"
