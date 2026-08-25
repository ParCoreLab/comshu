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

PCOMM=${1:?usage: object_access_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-object-access-XXXXXX")
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
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,id,name,range_base,range_size,device
0,2,0,100,0,900,0,5,0,20480,4096,0
1,2,0,100,0,900,0,9,0,20480,4096,1
2,2,0,100,0,900,0,7,0,20480,4096,2
1,2,0,100,0,900,0,1,0,8192,2048,1
1,2,0,100,0,900,0,2,0,8192,512,1
1,2,0,100,0,900,0,4,0,0,256,1
0,2,0,100,0,900,0,3,0,4096,1024,0
CSV

cat > "$WORK/ib_modify_qp.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,qp,qp_num,dest_qp_num,dest_lid,src_lid,state
0,1,0,200,0,0,0,1,5,6,11,10,2
1,1,0,200,0,0,0,2,6,5,10,11,2
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

# Every placement in one value. Each defect in the header changes it.
expect "the placements" \
    "SELECT string_agg(source||':'||owner_rank||':'||object||':'||obj_offset||':'||size||':'||exact, ' ' ORDER BY source, owner_rank, object, obj_offset, size) FROM object_access" \
    "p2p:1:9:0:4:false p2p:1:9:0:8:false p2p:1:9:256:8:false p2p:2:7:0:8:false p2p:2:7:256:8:false verbs:1:1:808:16:true verbs:1:2:64:32:true"

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
    "SELECT count(*) FROM object_access WHERE source = 'verbs'" "2"

# RDMA names the peer's real virtual address, so the verbs offset is exact. A
# p2p offset is anchored to the lowest address observed, so it is not — and the
# panel renders "(approx.)" off this column.
expect "verbs is exact and p2p is not" \
    "SELECT string_agg(DISTINCT source||'='||exact, ',' ORDER BY source||'='||exact) FROM object_access" \
    "p2p=false,verbs=true"

# Two accesses could not be placed, for two different reasons. An empty Objects
# panel must read as a count here, not as "this workload touched nothing".
expect "unplaceable accesses are counted, not dropped" \
    "SELECT n FROM link_health WHERE finding = 'unattributed_p2p_accesses'" "2"

# A local access is still an access into an object — it is only the PEER ones
# that need an owner resolved — so rank 1's own store must be placed.
expect "a local access is placed against its own rank" \
    "SELECT count(*) FROM object_access WHERE source = 'p2p' AND actor_rank = 1 AND owner_rank = 1" "1"

[ "$fail" -eq 0 ] || exit 1
echo "ok: object attribution holds on a capture built to break it"
