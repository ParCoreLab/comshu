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
5,1,0,100,0,0,0,333
6,1,0,100,0,0,0,333
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
2,2,0,1550,0,1560,0,3,12,4096,7,0,5,2,0,1
3,2,0,1600,0,1610,0,9,7,4096,7,8,9,2,0,1
3,2,0,1700,0,1710,0,9,8,4096,7,8,9,2,0,1
3,2,0,1800,0,1810,0,9,9,4096,7,8,9,2,0,1
3,2,0,1900,0,1910,0,9,10,4096,7,256,9,2,0,1
3,2,0,2000,0,2010,0,9,11,4096,7,256,9,2,0,1
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
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,laddr,raddr,qp_num,wqe_idx,lkey,rkey,length,dest_dct,opcode,decode,av_ds
1,2,0,2100,0,2110,0,8192,4096,30,0,3,7,200,0,8,0,0
1,2,0,2200,0,2210,0,8192,4096,30,1,3,7,200,0,8,0,0
1,2,0,2300,0,2310,0,8192,0,30,2,3,0,0,0,35,0,0
1,2,0,2400,0,2410,0,0,0,30,3,0,0,0,0,200,3,0
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
# `device` is kUnknownDevice (65535) throughout: this lane does not read it, but
# these CSVs are bound to the RAW column contract, so a record gaining a field
# means every fixture row for it gains a column. A stale fixture does not error
# — the import just produces no rows, and every assertion below reads back a
# column header instead of a value.
#
# EVERY ROW IS A CLAIM ABOUT THE PREDICATES.
#   rank 0 ep 200  two sends, 100 + 200 units          -> 0>1, 2 sends, 300
#   rank 0 ep 200  one send with status_class = 1      -> EXCLUDED (rejected)
#   rank 1 ep 201  one send, 50 units                  -> 1>0, 1 send, 50
#   rank 0 ep 202  one send, 7 units                   -> 0>-1, ambiguous prefix
#
# The error row is 9999 units so that counting it is loud rather than subtle.
cat > "$WORK/ucx_tag_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,buffer,request,count,tag,status,status_class,door,device
0,2,0,1000,0,1010,0,200,1,0,100,44,0,0,0,65535
0,2,0,1020,0,1030,0,200,1,0,200,44,0,2,0,65535
0,2,0,1040,0,1050,0,200,1,0,9999,44,-3,1,0,65535
1,2,0,1060,0,1070,0,201,1,0,50,44,0,0,0,65535
0,2,0,1080,0,1090,0,202,1,0,7,44,0,0,0,65535
CSV

# An active message adds to the same edge: payload only, since UcxAmSend keeps
# header_length separate on purpose and a header is control traffic.
cat > "$WORK/ucx_am_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,buffer,request,am_id,count,header_length,status,status_class,device
0,2,0,1100,0,1110,0,200,1,0,7,25,999,0,0,65535
CSV

# op: 0 Put, 1 Get.
#   rank 1 ep 201 PUT 30  -> outbound, joins 1>0
#   rank 2 ep 203 GET 80  -> INBOUND: rank 2 issues it, the bytes come FROM
#                            rank 0, so the edge is 0>2 and not 2>0
# A lane that ignores op counts the get as outbound and reverses that edge.
cat > "$WORK/ucx_rma.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,endpoint,buffer,request,rkey,count,remote_addr,status,status_class,op,door,device
1,2,0,1200,0,1210,0,201,1,0,9,30,4096,0,0,0,0,65535
2,2,0,1300,0,1310,0,203,1,0,9,80,4096,0,0,1,0,65535
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

# --- the copy-engine lane ---------------------------------------------------
#
# cuda_memcpy (534) is the only record for traffic no instrument in any other
# family can see, and its edge rule is unlike every other lane's, so it needs a
# fixture of its own rather than a row added to one above.
#
# THE RULE UNDER TEST: **a copy is an edge iff both device ordinals are known
# and they differ.** Not `kind = PeerToPeer`. `kind` is what the API was ASKED
# for and the ordinals are what the hardware DID, and rows 7 and 8 below are the
# two ways those disagree — a cudaMemcpyPeer whose two arguments name the same
# GPU (kind says peer, nothing crossed) and a cudaMemcpy(DeviceToDevice) between
# buffers on two different GPUs under unified addressing (kind says local, bytes
# crossed). A lane keyed on `kind` gets both of them wrong, in opposite
# directions, and no capture in the corpus contains either — which is exactly
# why they are written here.
#
# 65535 is kUnknownDevice, i.e. a HOST end. Rows 4-6 are H2D, D2H and a
# same-device D2D: real copies, real bytes, and NOT rank->rank traffic. They
# carry 4096 so that a leak trips the `bytes >= 999` canary the verbs lane
# already asserts, rather than hiding inside a plausible total.
#
#   1,2  rank 0, dev 0 -> dev 1, 100 and 200 B    -> 0>1, 2 sends, 300 B
#   3    rank 1, dev 1 -> dev 0, 50 B             -> 1>0
#   4    rank 0, host  -> dev 0, 4096 B  (H2D)    -> NOT an edge
#   5    rank 0, dev 0 -> host,  4096 B  (D2H)    -> NOT an edge
#   6    rank 0, dev 0 -> dev 0, 4096 B  (D2D)    -> NOT an edge, one device
#   7    rank 1, dev 1 -> dev 1, 8888 B, kind=5   -> NOT an edge, DESPITE kind
#   8    rank 3, dev 1 -> dev 0, 32 B,   kind=3   -> IS an edge, DESPITE kind
#   9    rank 2, dev 0 -> dev 1, 64 B             -> 2>3 on node 222, not 2>1
#  10,11 rank 4, dev 0 -> dev 1, 7 B x2           -> no node: BOTH ends -1
#  12    rank 5, dev 0 -> dev 1, 16 B, ctx 0      -> 5>6, on node 333
#  13    rank 6, dev 1 -> dev 0, 24 B, ctx 1      -> 6>5
#
# ROWS 12 AND 13 ARE THE OWNERSHIP TEST, and ranks 5 and 6 exist for nothing
# else. They are on a third node with NO mem_object row, NO gpu_access and NO
# gpu_kernel — a peer copy is the only thing they do — so the ONLY thing that can
# say which of them drives which GPU is `ctx_device`, the device whose context
# issued the copy. Without that column this pair resolves to `-1 -> -1`, which is
# exactly what four MPI ranks on du04 produced before it existed: a perfect
# device-space matrix and a comm_edges of one unresolved edge. src_device and
# dst_device cannot substitute — a peer copy names one device the rank owns and
# one it does not, and the row does not say which.
#
# Row 10/11 resolves to -1 on BOTH sides, which is unlike the nvlink lane's
# unresolved rows (there the actor's own rank is known because the access ran on
# its SM). A peer copy names two devices and the issuing rank need not own
# either, so when the node is unknown neither end can be named. That is the
# honest answer and unresolved_device_copy is what counts it.
#
# kind: 1 HostToDevice, 2 DeviceToHost, 3 DeviceToDevice, 5 PeerToPeer.
# src_space/dst_space: 2 Pinned, 3 Device. source: 0 Cupti.
cat > "$WORK/cuda_memcpy.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,correlation,bytes,stream,src_device,dst_device,ctx_device,kind,src_space,dst_space,source,async
0,2,0,4000,0,4010,0,1,100,7,0,1,0,5,3,3,0,1
0,2,0,4020,0,4030,0,2,200,7,0,1,0,5,3,3,0,1
1,2,0,4040,0,4050,0,3,50,7,1,0,1,5,3,3,0,1
0,2,0,4060,0,4070,0,4,4096,7,65535,0,0,1,2,3,0,0
0,2,0,4080,0,4090,0,5,4096,7,0,65535,0,2,3,2,0,0
0,2,0,4100,0,4110,0,6,4096,7,0,0,0,3,3,3,0,1
1,2,0,4120,0,4130,0,7,8888,7,1,1,1,5,3,3,0,1
3,2,0,4140,0,4150,0,8,32,7,1,0,1,3,3,3,0,1
2,2,0,4160,0,4170,0,9,64,7,0,1,0,5,3,3,0,1
4,2,0,4180,0,4190,0,10,7,7,0,1,0,5,3,3,0,1
4,2,0,4200,0,4210,0,11,7,7,0,1,0,5,3,3,0,1
5,2,0,4220,0,4230,0,12,16,7,0,1,0,5,3,3,0,1
6,2,0,4240,0,4250,0,13,24,7,1,0,1,5,3,3,0,1
CSV

# Two anchors per rank, so timeline_span has a line to carry ticks onto the wall
# clock with. Added for the copy lane's sake and it is not incidental: without a
# clock_anchor row EVERY lane of timeline_span is empty, so a timeline assertion
# written against this fixture would have passed vacuously at zero rows. That is
# the defect tests/hpc/suite/README.md gives its own heading to, and it fired
# here on the first run of these very checks.
#
# 1 ns/tick and identical on every rank: this fixture is not about the clock fit,
# and a per-rank slope would only make the expected values arithmetic nobody can
# check by eye.
cat > "$WORK/clock_anchor.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,domain,local_ticks,wall_ns,mono_ns
0,1,0,1000,0,0,0,0,1000,1000000000,500
0,1,0,9000,0,0,0,0,9000,1000008000,8500
1,1,0,1000,0,0,0,0,1000,1000000000,500
1,1,0,9000,0,0,0,0,9000,1000008000,8500
2,1,0,1000,0,0,0,0,1000,1000000000,500
2,1,0,9000,0,0,0,0,9000,1000008000,8500
3,1,0,1000,0,0,0,0,1000,1000000000,500
3,1,0,9000,0,0,0,0,9000,1000008000,8500
4,1,0,1000,0,0,0,0,1000,1000000000,500
4,1,0,9000,0,0,0,0,9000,1000008000,8500
5,1,0,1000,0,0,0,0,1000,1000000000,500
5,1,0,9000,0,0,0,0,9000,1000008000,8500
6,1,0,1000,0,0,0,0,1000,1000000000,500
6,1,0,9000,0,0,0,0,9000,1000008000,8500
CSV

# --- "the copy lane is absent, not empty" -----------------------------------
#
# link_health.memcpy_capture_unavailable reads PCOMM_CUPTI_ALLOWED out of
# Invocation's captured environment, because that is the one place a capture
# records that the launcher refused CUPTI to protect a gpumem run. Two ranks,
# one with it cleared and one with it set, so the row is proved to count the
# refused rank and only that one — a count that fired on both would be
# indistinguishable from `count(*) FROM invocation`.
#
# handle 2 is `env` in both rows and resolves per rank, which is the point of
# _intern_name's (rank, domain, handle) key: the same handle names different
# text on different ranks.
cat > "$WORK/invocation.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,argv,env,providers,build,images,pid
0,0,0,0,0,0,0,1,2,3,4,5,1001
1,0,0,0,0,0,0,1,2,3,4,5,1002
CSV

cat > "$WORK/intern_text.csv" <<'CSV'
rank,domain,handle,text
0,2,1,./app
0,2,2,PCOMM_CUPTI_ALLOWED=0
0,2,3,gpumem
0,2,4,build
0,2,5,images
1,2,1,./app
1,2,2,PCOMM_CUPTI_ALLOWED=1
1,2,3,ibverbs
1,2,4,build
1,2,5,images
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
    "-1>-1:cudamemcpy:2:14 0>1:cudamemcpy:2:300 1>0:cudamemcpy:1:50 2>3:cudamemcpy:1:64 3>2:cudamemcpy:1:32 5>6:cudamemcpy:1:16 6>5:cudamemcpy:1:24 1>3:devx:2:400 0>1:nvlink:3:48 1>0:nvlink:4:28 2>3:nvlink:1:64 4>-1:nvlink:2:14 0>-1:ucx:1:7 0>1:ucx:3:325 0>2:ucx:1:80 1>0:ucx:2:80 0>2:verbs:3:300 1>3:verbs:2:100 2>0:verbs:2:64 3>-1:verbs:5:536"

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
    "cudamemcpy,devx,nvlink,ucx,verbs"

# NVLink is counted in ACCESSES, verbs in MESSAGES, UCX in DATATYPE UNITS.
# Summing them would be a category error, and the unit column is what stops a
# panel doing it. UcxTagSend.count is a UCP datatype count and equals bytes only
# for a contiguous 8-bit datatype, which the capture does not record.
expect "the unit says which is which" \
    "SELECT string_agg(DISTINCT transport||'='||unit, ',' ORDER BY transport||'='||unit) FROM comm_edges" \
    "cudamemcpy=bytes,devx=messages,nvlink=accesses,ucx=datatype-units,verbs=messages"

# --- the copy lane, one clause at a time ------------------------------------
#
# THE CLAUSE NO OTHER LANE HAS. `kind` is the API's claim and the ordinals are
# the hardware's, and these two rows are the only place either disagreement is
# exercised. 8888 is chosen so that leaking row 7 into the matrix is unmissable
# — it would also trip the `bytes >= 999` canary above, which is the point of
# picking it rather than a plausible number.
expect "a peer-KIND copy that never left the device is not an edge" \
    "SELECT count(*) FROM comm_edges WHERE transport = 'cudamemcpy' AND bytes >= 8888" "0"

# The inverse, and the one that would be a SILENT hole rather than a loud
# addition: a cudaMemcpy(..., DeviceToDevice) between buffers on two different
# GPUs under unified addressing moves bytes across a device boundary while
# calling itself local. Keyed on `kind` this edge disappears entirely.
expect "a local-KIND copy across two devices IS an edge" \
    "SELECT sends||':'||bytes FROM comm_edges
      WHERE transport = 'cudamemcpy' AND src_rank = 3 AND dst_rank = 2" "1:32"

# H2D, D2H and same-device D2D are real copies and real bytes, and none of them
# is rank->rank traffic. Three rows of 4096 that must not appear anywhere in the
# matrix; the sum over the whole lane is stated so a leak of any ONE of them
# fails rather than only a leak of all three.
expect "host staging and same-device copies are not edges" \
    "SELECT coalesce(sum(bytes), 0) FROM comm_edges WHERE transport = 'cudamemcpy'" "500"

# THE OWNERSHIP COLUMN, on the one pair of ranks that has nothing else. Ranks 5
# and 6 have a node and a peer copy each and no mem_object, no gpu_access and no
# gpu_kernel — so this edge exists only if `ctx_device` is admitted as evidence
# of which rank drives which GPU. Stated as the two-edge string rather than as a
# count, because dropping the column gives `-1 -> -1` and a count would still be
# 2.
expect "a rank's own device is learned from the copy's context" \
    "SELECT string_agg(src_rank||'>'||dst_rank||':'||bytes, ' ' ORDER BY src_rank)
     FROM comm_edges WHERE transport = 'cudamemcpy' AND (src_rank >= 5 OR dst_rank >= 5)" \
    "5>6:16 6>5:24"

# Rank 2 and rank 1 both own a 'device 1'. A copy on node 222 must land on
# node 222, exactly as the nvlink lane's does — the device ordinal is only an
# identity with its node.
expect "copy device ordinals are scoped by node" \
    "SELECT dst_rank FROM comm_edges
      WHERE transport = 'cudamemcpy' AND src_rank = 2" "3"

# Both ends unresolvable, counted rather than dropped or guessed. Rank 4 has
# peer-copy traffic and no node identity, which is the pure-CUDA capture's
# shape: no LID to derive a node from and no NVSHMEM to name a peer.
expect "copies with no resolvable node are counted" \
    "SELECT n FROM link_health WHERE finding = 'unresolved_device_copy'" "2"

# THE ABSENT-VERSUS-EMPTY ROW. Rank 0's capture was taken with the launcher
# holding CUPTI back for gpumem, rank 1's was not. Counting both would make the
# row `count(*) FROM invocation` wearing a costume.
expect "a capture that could not record copies says so" \
    "SELECT n FROM link_health WHERE finding = 'memcpy_capture_unavailable'" "1"

# The copy lane on the shared timeline, including the rows that are NOT edges:
# a D2H is not communication but it is absolutely a thing that happened at a
# time, and dropping it from the timeline would leave a rank looking idle while
# it was blocked in a copy. Eleven rows in, eleven rows out.
expect "every copy reaches the timeline, edge or not" \
    "SELECT count(*) FROM timeline_span WHERE stream = 'cuda_memcpy'" "13"

# ... and it carries bytes, which is what makes the lane readable as work
# rather than as a bare mark.
# 4096*3 + 8888 + 300 + 50 + 64 + 32 + 14 + 16 + 24 = 21676.
expect "the timeline copy lane carries the bytes" \
    "SELECT sum(bytes) FROM timeline_span WHERE stream = 'cuda_memcpy'" "21676"

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
    "1>3:2:400 0>2:3:300 1>3:2:100 2>0:2:64 3>-1:5:536"

# --- edge_sizes: the same edges, one group-by deeper -------------------------
#
# THE INVARIANT THAT MAKES THE DUPLICATION SAFE. edge_sizes repeats comm_edges'
# peer resolution and scope expressions over size-bucketed source views rather
# than sharing them, deliberately — comm_edges is checked against a second C++
# implementation (CsvAgg) and rewriting how it is computed to add a table beside
# it is the wrong risk. The price of that choice is two copies of one rule, and
# this is what stops them drifting: rolled up over size_class, edge_sizes must
# reproduce comm_edges EXACTLY, on every column and every lane.
#
# Stated as a set difference in both directions, so a row present in one and
# absent from the other fails just as loudly as a row whose numbers disagree.
expect "edge_sizes rolls up to comm_edges exactly" \
    "SELECT count(*) FROM (
       SELECT src_rank, dst_rank, transport, unit, scope, sum(sends) AS sends, sum(bytes) AS bytes
       FROM edge_sizes GROUP BY 1,2,3,4,5
       EXCEPT SELECT src_rank, dst_rank, transport, unit, scope, sends, bytes FROM comm_edges
     )" "0"

expect "and comm_edges has no edge that edge_sizes lost" \
    "SELECT count(*) FROM (
       SELECT src_rank, dst_rank, transport, unit, scope, sends, bytes FROM comm_edges
       EXCEPT
       SELECT src_rank, dst_rank, transport, unit, scope, sum(sends), sum(bytes)
       FROM edge_sizes GROUP BY 1,2,3,4,5
     )" "0"

# THE WHOLE POINT OF THE TABLE: sizes comm_edges summed away are back.
#
# rank 3's queue pair posts three 8-byte sends and two 256-byte ones — MIXED
# SIZES ON ONE EDGE, which no other pair in this fixture had, because until this
# table existed nothing could tell them apart and there was no reason to build
# one. comm_edges reports the edge as `5 sends, 536 bytes`; this splits it.
# 256 rather than something larger so the total stays under the 999 sentinel the
# local-access clause above depends on.
expect "a rank pair's message sizes are separable" \
    "SELECT string_agg(size_class||':'||sends, ' ' ORDER BY size_class)
     FROM edge_sizes WHERE transport = 'verbs' AND src_rank = 3" \
    "8:3 256:2"

# The same, on the copy lane, where `size_class` means something different and
# better: real transfer bytes, not one access's width as on nvlink. Rank 0's two
# peer copies are 100 and 200 B, so the edge comm_edges reports as `2 sends, 300
# bytes` splits into two classes — which is the question that made this table
# exist, asked of the newest lane.
expect "a copy edge's transfer sizes are separable" \
    "SELECT string_agg(size_class||':'||sends||':'||bytes, ' ' ORDER BY size_class)
     FROM edge_sizes WHERE transport = 'cudamemcpy' AND src_rank = 0 AND dst_rank = 1" \
    "64:1:100 128:1:200"

# A zero-length operation is class 0 and is KEPT. Dropping it would make `sends`
# here disagree with comm_edges, which the roll-up above would catch — this
# names the cause so a failure says which rule broke.
expect "size classes are lower bounds of power-of-two buckets" \
    "SELECT count(*) FROM edge_sizes
      WHERE size_class <> 0 AND (size_class & (size_class - 1)) <> 0" "0"

# A ZERO-LENGTH OPERATION IS CLASS 0 AND IS KEPT. rank 2 posts one 64-byte send
# and one of length 0 — an RDMA write with no payload is still a message, and
# dropping it would make `sends` here disagree with comm_edges. The roll-up
# above would catch that; this names the cause, and the fixture carries the row
# so the rule is exercised rather than merely written.
expect "a zero-length operation keeps its own class" \
    "SELECT string_agg(size_class||':'||sends||':'||bytes, ' ' ORDER BY size_class)
     FROM edge_sizes WHERE transport = 'verbs' AND src_rank = 2 AND dst_rank = 0" \
    "0:1:0 64:1:64"

[ "$fail" -eq 0 ] || exit 1
echo "ok: comm matrix holds on a capture built to break it"
