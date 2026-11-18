#!/usr/bin/env bash
# object_life + object_access's lifetime choice, on a fixture built to break them
# (PLAN-20261118-2130).
#
# An object's id is interned from its base address, so a freed-and-reallocated
# address is ONE id for TWO buffers. object_life splits them into incarnations
# and object_access credits each access to the one live when it happened. Every
# way that could be wrong while still producing plausible rows is a row here:
#
#   - comparing raw TICKS across ranks. Rank 1's clock sits 5 s after rank 0's
#     on the wall, so a put timed on rank 0 must be converted before it is
#     compared with rank 1's buffer — raw ticks put every put in the wrong
#     incarnation;
#   - one incarnation seen by TWO emitters (the engine's cuMemAlloc door and
#     gpumem's NVBit callback both record a cudaMalloc) counted as two, and the
#     second emitter's close then ending the NEXT incarnation;
#   - an access in the GAP between incarnations: placed in the nearer one and
#     flagged in_window = FALSE, never dropped and never claimed live;
#   - the cross-rank slack (1 ms for NTP skew): a put just before the second
#     incarnation's open is inside it; a same-rank send gets no slack.
set -u
COMSHU=${1:?usage: object_life_rule.sh <comshu-binary>}
if [ ! -x "$COMSHU" ]; then echo "SKIP: no comshu binary at $COMSHU"; exit 0; fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-object-life-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
T=rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value

cat > "$WORK/node_identity.csv" <<CSV
$T,node_hash
0,1,0,100,0,0,0,111
1,1,0,100,0,0,0,111
CSV

# Rank 0: 1 tick = 1000 ns, wall 0 at tick 0.
# Rank 1: 1 tick = 1000 ns, wall 5 s at tick 0 — five seconds "later".
cat > "$WORK/clock_anchor.csv" <<CSV
$T,domain,local_ticks,wall_ns,mono_ns
0,1,0,0,0,0,0,0,0,0,0
0,1,0,1000000,0,0,0,0,1000000,1000000000,0
1,1,0,0,0,0,0,0,0,5000000000,0
1,1,0,1000000,0,0,0,0,1000000,6000000000,0
CSV

# Rank 1, object 31 at base 95000, two incarnations:
#   life 1: 256 B, ticks [100, 50000)  = wall [5.0001 s, 5.05 s). Recorded by
#           TWO emitters (ticks 100 and 101, one extent), closed by both
#           (50000, then the second emitter's 50001 — which must end nothing).
#   life 2: 1024 B, from tick 60000 = wall 5.06 s, never closed.
cat > "$WORK/mem_object.csv" <<CSV
$T,id,name,range_base,range_size,device,attr_status,origin
1,1,0,100,0,0,0,31,0,95000,256,0,0,7
1,1,0,101,0,0,0,31,0,95000,256,0,0,2
1,1,0,60000,0,0,0,31,0,95000,1024,0,0,2
CSV
cat > "$WORK/entity_close.csv" <<CSV
$T,subject,opener
1,3,0,50000,0,0,0,31,520
1,3,0,50001,0,0,0,31,520
CSV

# Rank 0 ep 50 -> rank 1; rank 1 ep 53 -> rank 0.
cat > "$WORK/ucx_worker_address.csv" <<CSV
$T,worker,addr_hash,prefix_hash,addr_len
0,1,0,100,0,0,0,1,161,160,64
1,1,0,100,0,0,0,1,177,176,64
CSV
cat > "$WORK/ucx_ep_connection.csv" <<CSV
$T,endpoint,remote_prefix_hash
0,1,0,100,0,0,0,50,176
1,1,0,100,0,0,0,53,160
CSV
cat > "$WORK/intern_key.csv" <<CSV
$T,key,handle,domain
1,0,0,0,0,0,0,95100,80,1
CSV

# PUTS from rank 0 into rank 1's buffer (offset 10), timed on RANK 0's clock:
#   A tick 5,020,000 = 5.02 s    inside life 1
#   B tick 5,057,000 = 5.057 s   in the gap, beyond either slack: nearer life 2
#                                (3 ms vs 7 ms), in_window FALSE
#   C tick 5,059,500 = 5.0595 s  0.5 ms before life 2 opens: inside the slack
#   D tick 5,100,000 = 5.1 s     inside life 2
# Compared as raw ticks, all four are "after" everything on rank 1 and land in
# life 2 — A is the row that catches it.
cat > "$WORK/ucx_rma.csv" <<CSV
$T,endpoint,buffer,request,rkey,count,remote_addr,status,status_class,op,door,device
0,0,0,5020000,0,0,0,50,0,0,0,8,95010,0,0,0,0,0
0,0,0,5057000,0,0,0,50,0,0,0,8,95010,0,0,0,0,0
0,0,0,5059500,0,0,0,50,0,0,0,8,95010,0,0,0,0,0
0,0,0,5100000,0,0,0,50,0,0,0,8,95010,0,0,0,0,0
CSV

# SENDS from rank 1's own buffer (offset 100, fits both incarnations, so only
# TIME can choose), on rank 1's clock — same rank, no slack:
#   E tick 30000  inside life 1
#   F tick 70000  inside life 2
#   G tick 56000  in the gap: nearer life 2 (4 ms vs 6 ms), in_window FALSE
cat > "$WORK/ucx_tag_send.csv" <<CSV
$T,endpoint,buffer,request,count,tag,status,status_class,door,device
1,0,0,30000,0,0,0,53,80,0,16,1,0,0,0,0
1,0,0,70000,0,0,0,53,80,0,16,1,0,0,0,0
1,0,0,56000,0,0,0,53,80,0,16,1,0,0,0,0
CSV

"$COMSHU" link "$WORK" > "$WORK/link.out" 2>&1
link_rc=$?
if grep -q "no DuckDB" "$WORK/link.out"; then echo "SKIP: build without DuckDB"; exit 0; fi
if [ "$link_rc" -ne 0 ] || grep -q "SOME FAILED" "$WORK/link.out"; then
    cat "$WORK/link.out"; echo "FAIL: link did not complete"; exit 1
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

expect "two emitters of one extent are one incarnation; a second close ends nothing" \
    "SELECT string_agg(life||':'||size||':'||emitters||':'||(close_ns IS NULL), ' ' ORDER BY life) FROM object_life WHERE rank = 1 AND object = 31" \
    "1:256:2:false 2:1024:1:true"

expect "windows are on the wall clock, per rank" \
    "SELECT string_agg(open_ns||'-'||coalesce(close_ns::VARCHAR, 'open'), ' ' ORDER BY life) FROM object_life WHERE rank = 1 AND object = 31" \
    "5000100000-5050000000 5060000000-open"

expect "each put lands in the incarnation live on the OWNER's clock" \
    "SELECT string_agg(life||':'||in_window, ' ' ORDER BY obj_offset, size, life, in_window) FROM (SELECT * FROM object_access WHERE source = 'ucx_rma') t" \
    "1:true 2:false 2:true 2:true"

expect "each send lands in the incarnation live when it was sent; the gap is flagged" \
    "SELECT string_agg(life||':'||in_window, ' ' ORDER BY life, in_window) FROM object_access WHERE source = 'ucx_send'" \
    "1:true 2:false 2:true"

expect "nothing is dropped" \
    "SELECT count(*) FROM object_access" "7"

[ "$fail" -eq 0 ] || exit 1
echo "ok: object lifetimes hold on a capture built to break them"
