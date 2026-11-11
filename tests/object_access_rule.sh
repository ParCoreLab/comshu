#!/usr/bin/env bash
# The object-attribution rule, against a capture built to break it.
#
# WHY THIS EXISTS. object_access is what the Objects panel reads and it had no
# local test: every assertion over it lived in tests/hpc, so the rule that
# places a byte inside a buffer could only be regression-tested by booking MN5.
#
# It is also the derived table with the WORST history, and each defect below is
# one that actually shipped — the comments in link_sql.hpp record all four:
#
#   - owner_rank meant the writer in the p2p branch and the far end in the verbs
#     branch. One column, two meanings, so any panel grouping on it silently
#     mixed them. Measured on hubspoke-44465477: the hub's one recv buffer came
#     out as THREE objects of one writer each, which is precisely the property
#     that case exists to demonstrate.
#   - the p2p offset window partitioned on (rank, object), gluing one actor's
#     mappings of the same symmetric object on SEVERAL peers into one window. A
#     2048-byte object reported offsets up to 549,755,815,936.
#   - the object id was carried across ranks unchanged, though it is an intern
#     handle minted per rank. On nvshmem_coexist-44696991 id 701 named four
#     different buffers.
#   - the verbs branch resolved the address against the SENDER's objects.
#
# THE FIXTURE IS BUILT SO EACH HAS SOMETHING TO GET WRONG. Rank 0 maps the same
# symmetric object on two different peers at wildly separated local addresses;
# rank 1 holds two objects sharing a base so that "smallest containing wins" is
# a choice and not a tautology; and every rank's intern ids for the shared
# buffer deliberately DISAGREE, so carrying one across a rank boundary names the
# wrong thing rather than accidentally the right one.
set -uo pipefail

COMSHU=${1:?usage: object_access_rule.sh <comshu-binary>}
if [ ! -x "$COMSHU" ]; then echo "SKIP: no comshu binary at $COMSHU"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-object-access-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# One node, three GPUs, so every peer access has a nameable owner.
cat > "$WORK/node_identity.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,node_hash
0,1,0,100,0,0,0,111
1,1,0,100,0,0,0,111
2,1,0,100,0,0,0,111
CSV

# THE INTERN IDS DISAGREE ON PURPOSE. The buffer at [20480, 24576) is one
# logical symmetric object, and the three ranks call it 5, 9 and 7. The reader
# must translate through the OWNER's row by RANGE; carrying the actor's id
# across the boundary would report object 5, which on rank 1 names nothing.
#
# Rank 1 also holds two objects sharing a base: [8192,10240) and [8192,8704).
# An address inside both must resolve to the smaller, so "ORDER BY range_size
# LIMIT 1" is doing work rather than picking the only candidate.
#
# And rank 1 holds one object at base 0. That row exists ONLY so the
# remote_addr = 0 guard has something to fail into: without it the fixture
# cannot express that defect, because no object would contain address 0 and the
# range join would drop the row for the right answer by the wrong reason. This
# was found by deleting the guard and watching the test stay green — the same
# way wr_flow_rule.sh found that its own fixture could not see a dropped
# clause.
cat > "$WORK/mem_object.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,id,name,range_base,range_size,device,attr_status
0,2,0,100,0,900,0,5,0,20480,4096,0,0
1,2,0,100,0,900,0,9,0,20480,4096,1,0
2,2,0,100,0,900,0,7,0,20480,4096,2,0
1,2,0,100,0,900,0,1,0,8192,2048,1,0
1,2,0,100,0,900,0,2,0,8192,512,1,0
1,2,0,100,0,900,0,4,0,0,256,1,0
1,2,0,100,0,900,0,6,0,30000,2048,1,0
1,2,0,100,0,900,0,8,0,60000,1024,1,0
0,2,0,100,0,900,0,3,0,4096,1024,0,0
CSV

cat > "$WORK/ib_modify_qp.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,src_gid_prefix,src_gid_iface,dest_gid_prefix,dest_gid_iface,qp,qp_num,dest_qp_num,dest_lid,src_lid,state
0,1,0,200,0,0,0,0,0,0,0,1,5,6,11,10,2
1,1,0,200,0,0,0,0,0,0,0,2,6,5,10,11,2
CSV

# Rank 0 writes into rank 1's memory. The owner is rank 1 in both rows and the
# address is resolved against RANK 1's objects — rank 0 has nothing at 8256, so
# resolving against the sender makes both rows vanish rather than come out wrong.
#
# The third row has remote_addr 0: a two-sided SEND names no remote address, and
# subtracting a base from zero would manufacture an offset into whatever object
# happens to sit lowest.
cat > "$WORK/ib_post_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,qp,wr_id,remote_addr,rkey,length,qp_num,send_flags,opcode,num_sge
0,2,0,1000,0,1010,0,1,1,8256,7,32,5,2,0,1
0,2,0,1100,0,1110,0,1,2,9000,7,16,5,2,0,1
0,2,0,1200,0,1210,0,1,3,0,7,8,5,2,0,1
0,2,0,1300,0,1310,0,1,4,200,77,32,5,2,0,1
0,2,0,1400,0,1410,0,1,5,20,88,8,5,2,0,1
0,2,0,1500,0,1510,0,1,6,60100,99,4,5,2,0,1
0,2,0,1600,0,1610,0,1,7,30,99,4,5,2,0,1
CSV

# GPU-INITIATED SENDS, which reach the capture as decoded WQEs rather than as
# ibv_post_send. Rank 0 owns two DEVX queues to rank 1 (lids as above), and each
# row is built to be misplaced by one specific mistake:
#
#   20500  inside rank 1's object 9 AND rank 0's object 5 at the same range —
#          resolving against the SENDER names 5.
#   8256   inside both of rank 1's nested objects — must be the smaller, 2.
#   9100   only in rank 1's larger object 1, on the queue whose device is
#          unknown — actor_device must fall back to the rank's GPU (0), while
#          queue 7's sends must report the QUEUE's GPU (3), which no other
#          table assigns to rank 0.
#   4100   only in rank 0's object 3 — the sender's memory, so not placed.
#   0      raddr 0 names no buffer.
#   9000   twice, once as NVSHMEM's DUMP (opcode 35) and once truncated
#          (decode 2): neither is traffic, and _sends drops both, so this view
#          must too or the panel shows bytes comm_edges never counted.
cat > "$WORK/mlx5_devx_qp_connect.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,dest_gid_prefix,dest_gid_iface,src_gid_prefix,src_gid_iface,qp_num,dest_qp_num,dest_lid,src_lid,port_num
0,1,0,200,0,0,0,0,0,0,0,7,8,11,10,1
1,1,0,200,0,0,0,0,0,0,0,8,7,10,11,1
0,1,0,200,0,0,0,0,0,0,0,9,10,11,10,1
1,1,0,200,0,0,0,0,0,0,0,10,9,10,11,1
CSV

cat > "$WORK/mlx5_devx_qp.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,obj,sq_base,qp_num,wq_umem_id,log_sq_size,device,cqn_snd,service_type,gpu_driven
0,2,0,100,0,900,0,1,0,7,1,9,3,1,0,1
0,2,0,100,0,900,0,2,0,9,2,9,65535,2,0,1
CSV

cat > "$WORK/mlx5_device_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,laddr,raddr,qp_num,wqe_idx,lkey,rkey,length,dest_dct,opcode,decode,av_ds,submitter,dest_lid,pad,dest_gid_prefix,dest_gid_iface
0,1,0,2000,0,0,0,100,20500,7,0,1,2,8,0,20,1,0,2,0,0,0,0
0,1,0,2010,0,0,0,100,8256,7,1,1,2,32,0,8,0,0,2,0,0,0,0
0,1,0,2020,0,0,0,100,9100,9,0,1,2,4,0,8,0,0,2,0,0,0,0
0,1,0,2030,0,0,0,100,4100,7,2,1,2,4,0,8,0,0,2,0,0,0,0
0,1,0,2040,0,0,0,100,0,7,3,1,2,4,0,8,0,0,2,0,0,0,0
0,1,0,2050,0,0,0,100,9000,7,4,1,2,4,0,35,0,0,2,0,0,0,0
0,1,0,2060,0,0,0,100,9000,7,5,1,2,4,0,8,2,0,2,0,0,0,0
0,1,0,2070,0,0,0,100,0,7,6,1,77,16,0,8,0,0,2,0,0,0,0
0,1,0,2080,0,0,0,100,100,7,7,1,77,8,0,8,0,0,2,0,0,0,0
0,1,0,2090,0,0,0,100,10,7,8,1,88,4,0,8,0,0,2,0,0,0,0
CSV

# ZERO-BASED REGIONS — NCCL GIN's GDAKI window shape. A remote address naming
# such a region is an OFFSET, and the region's rkey is the only thing that says so:
#
#   rkey 77  rank 1's 2 KiB window at VA 30000, registered at iova 0 (the
#            iova row). Offsets 0, 100 and 200 must land in object 6 at those
#            offsets — including offset 0, which the raddr = 0 guard would
#            otherwise drop as a control send.
#   rkey 88  a zero-based dmabuf window whose fd comshu never resolved: base 0,
#            no VA. Offset 10/20 resolved as an ADDRESS falls into object 4,
#            the one at base 0, with exact = TRUE. It must be dropped, and
#            counted in link_health.
#   rkey 7   reused by two ORDINARY regions at different bases — the common
#            case. It must stay a plain address and not be taken for ambiguous.
#   rkey 99  reused by an ordinary region at 60000 AND an unresolved zero-based
#            one — NCCL's proxy plane and GIN sharing keys (MN5 48812008). A
#            send at 60100 lies inside the ordinary region, so it is a VA and
#            is placed; a send at 30 lies in no ordinary region of the key, so
#            it may be an offset and is dropped.
cat > "$WORK/ib_mem_region.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,mr,pd,range_base,range_size,lkey,rkey,access,dmabuf_fd,origin
1,0,0,0,0,0,0,50,1,30000,2048,77,77,15,9,2
1,0,0,0,0,0,0,51,1,0,4096,88,88,15,10,2
1,0,0,0,0,0,0,53,1,8192,2048,7,7,15,-1,1
1,0,0,0,0,0,0,54,1,4096,1024,7,7,15,-1,1
1,0,0,0,0,0,0,60,1,60000,1024,99,99,15,-1,1
1,0,0,0,0,0,0,61,1,0,4096,99,99,15,11,2
CSV

cat > "$WORK/ib_mem_region_iova.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,mr,rkey,iova
1,0,0,0,0,0,0,50,77,0
CSV

# op 1 = Store, space 0 = Global.
#
# Rank 0 maps the symmetric object on BOTH peers, and its two mappings are a
# billion bytes apart — which is the shape of the real thing, since each peer's
# copy is mapped wherever the driver put it. Each mapping must get its OWN
# offset window: partitioning on (rank, object) instead of
# (rank, mem_device, object) takes one minimum across both and turns the
# device-2 offsets into ~1e9, inside a 4096-byte object.
#
# The last two rows are unplaceable, each for a different reason, and both must
# be COUNTED rather than dropped into an empty panel that reads as "this
# workload touched nothing":
#   object 99 — rank 0 has no such object, so there is nothing to translate
#   mem_device 5 — no GPU 5 on this node, so the owning rank is unnameable
cat > "$WORK/gpu_access.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,pc,object,address,launch,size,op,space,run_device,mem_device,thread
0,3,0,3000,0,3100,0,1,5,20736,0,8,1,0,0,1,0
0,3,0,3000,0,3100,0,1,5,20992,0,8,1,0,0,1,0
0,3,0,3000,0,3100,0,1,5,1000020736,0,8,1,0,0,2,0
0,3,0,3000,0,3100,0,1,5,1000020992,0,8,1,0,0,2,0
1,3,0,3000,0,3100,0,1,9,20608,0,4,1,0,1,1,0
0,3,0,3000,0,3100,0,1,99,777,0,8,1,0,0,1,0
2,3,0,3000,0,3100,0,1,7,5000,0,8,1,0,2,5,0
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

# Every placement in one value. Each defect in the header changes it.
expect "the placements" \
    "SELECT string_agg(source||':'||owner_rank||':'||object||':'||obj_offset||':'||size||':'||exact, ' ' ORDER BY source, owner_rank, object, obj_offset, size) FROM object_access" \
    "devx:1:1:908:4:true devx:1:2:64:32:true devx:1:6:0:16:true devx:1:6:100:8:true devx:1:9:20:8:true p2p:1:9:0:4:false p2p:1:9:0:8:false p2p:1:9:256:8:false p2p:2:7:0:8:false p2p:2:7:256:8:false verbs:1:1:808:16:true verbs:1:2:64:32:true verbs:1:6:200:32:true verbs:1:8:100:4:true"

# The column that meant two things. A peer write is owned by the rank whose
# memory it landed in, never by the rank that issued it.
expect "a peer write is never owned by its writer" \
    "SELECT count(*) FROM object_access WHERE source = 'p2p' AND actor_rank = 0 AND owner_rank = 0" "0"

# Rank 0 calls the shared buffer 5; rank 1 calls it 9. The reader reports the
# owner's name for it, because that is the only one that means anything on the
# owner's rank.
expect "the object id is the owner's, not the actor's" \
    "SELECT string_agg(DISTINCT object, ',' ORDER BY object) FROM object_access WHERE source = 'p2p'" \
    "7,9"

# The partition bug in one number. Both mappings are of a 4096-byte object, so
# no offset can reach 4096 — gluing them gives ~1e9.
expect "each peer mapping gets its own offset window" \
    "SELECT max(obj_offset) FROM object_access WHERE source = 'p2p'" "256"

expect "no offset escapes its object" \
    "SELECT count(*) FROM object_access WHERE obj_offset >= 4096 AND source = 'p2p'" "0"

# 8256 is inside both [8192,10240) and [8192,8704). The smaller one is the
# better answer and the query has to choose it rather than take either.
expect "the smallest containing object wins" \
    "SELECT object FROM object_access WHERE source = 'verbs' AND obj_offset = 64" "2"

# A two-sided SEND names no remote address. Placing it would invent an offset.
expect "a send with no remote address is not placed" \
    "SELECT count(*) FROM object_access WHERE source = 'verbs' AND object NOT IN (6, 8)" "2"

# RDMA names the peer's real virtual address, so the verbs offset is exact. A
# p2p offset is anchored to the lowest address observed, so it is not — and the
# panel renders "(approx.)" off this column.
expect "verbs is exact and p2p is not" \
    "SELECT string_agg(DISTINCT source||'='||exact, ',' ORDER BY source||'='||exact) FROM object_access" \
    "devx=true,p2p=false,verbs=true"

# Two accesses could not be placed, for two different reasons. An empty Objects
# panel must read as a count here, not as "this workload touched nothing".
expect "unplaceable accesses are counted, not dropped" \
    "SELECT n FROM link_health WHERE finding = 'unattributed_p2p_accesses'" "2"

# A local access is still an access into an object — it is only the PEER ones
# that need an owner resolved — so rank 1's own store must be placed.
expect "a local access is placed against its own rank" \
    "SELECT count(*) FROM object_access WHERE source = 'p2p' AND actor_rank = 1 AND owner_rank = 1" "1"

# The devx branch, one claim per defect in the fixture comment above.
expect "a gpu put resolves against the PEER's objects" \
    "SELECT count(*) FROM object_access WHERE source = 'devx' AND owner_rank = 0" "0"

expect "a gpu put names the queue's GPU, else the rank's" \
    "SELECT string_agg(obj_offset||'@'||actor_device, ',' ORDER BY obj_offset) FROM object_access WHERE source = 'devx'" \
    "0@3,20@3,64@3,100@3,908@0"

# Seven decoded rows, three placed: the sender's own memory, raddr 0, DUMP and a
# truncated WQE are all out. Counting the DUMP or the truncated row here would
# put bytes in the Objects panel that comm_edges does not count.
expect "only traffic rows that name a peer buffer are placed" \
    "SELECT count(*) FROM object_access WHERE source = 'devx' AND object <> 6" "3"

# Zero-based regions. Every offset into rkey 77 lands in the window at its own
# offset, offset 0 included; nothing naming rkey 88 lands anywhere — above all
# not in object 4, the one an offset read as an address would hit.
expect "an offset into a zero-based region is placed at that offset" \
    "SELECT string_agg(source||':'||obj_offset, ',' ORDER BY source, obj_offset) FROM object_access WHERE object = 6" \
    "devx:0,devx:100,verbs:200"

expect "an offset into an unresolved region is never read as an address" \
    "SELECT count(*) FROM object_access WHERE owner_rank = 1 AND object = 4" "0"

expect "the dropped offsets are counted, not silent" \
    "SELECT n FROM link_health WHERE finding = 'unplaced_offset_sends'" "3"

# rkey 7, reused across two ordinary regions, needs no check of its own: both
# verbs rows in "the placements" name it, and either one dropped as ambiguous
# would change that value.

[ "$fail" -eq 0 ] || exit 1
echo "ok: object attribution holds on a capture built to break it"
