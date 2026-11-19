#!/usr/bin/env bash
# Two queue pairs on ONE rank with ONE qp_num: each send goes to its own queue
# pair's peer, never a splice of the two.
#
# qp_num is allocated per HCA, and a rank driving several HCAs holds queue pairs
# that share a number. MN5 job 49210863 (case 12) had exactly this on rank 1:
# qp 61301 was a CPU proxy QP connected to itself over LID 7837 AND a GPU QP on
# LID 7821 connected to rank 0. Keyed by (rank, qp_num), the derived layer took
# max() over the two connects and resolved 672 GPU sends to no one; the CSV
# reader let the loopback's connect win and printed 49 of them as 1 -> 1.
#
# The capture now names the queue pair by its DEVX object on both records
# (mlx5_devx_qp_connect.qp_obj, mlx5_device_send.qp_obj), and this fixture is
# that case in miniature:
#
#   rank 1 qp 61301 obj 467   LID 7837, connected to 7837/61302 (itself)
#   rank 1 qp 61302 obj 468   LID 7837, connected to 7837/61301 (itself)
#   rank 1 qp 61301 obj 1340  LID 7821, connected to 7820/69664 (rank 0), GPU 2
#   rank 0 qp 69664 obj 900   LID 7820, connected to 7821/61301 (rank 1)
#
# Sends:
#   rank 1 obj 1340 x3 (10 B each)  -> rank 0
#   rank 1 obj 467  x1 (5 B)        -> rank 1 (a real loopback, and allowed)
#   rank 1 qp 61301 obj 0 x1 (7 B)  -> unresolved: the door could not say which
#                                      of two namesakes, and nothing is guessed
#   rank 0 qp 69664 obj 0 x1 (11 B) -> rank 1: one queue pair by that number on
#                                      rank 0, so the number is enough
set -u
COMSHU=${1:?usage: qp_namesake_rule.sh <comshu-binary>}
if [ ! -x "$COMSHU" ]; then echo "SKIP: no comshu binary at $COMSHU"; exit 0; fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-qp-namesake-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
T=rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value

cat > "$WORK/mlx5_devx_qp.csv" <<CSV
$T,obj,sq_base,qp_num,wq_umem_id,log_sq_size,device,cqn_snd,service_type,gpu_driven
1,2,0,100,0,0,0,467,0,61301,1,9,65535,1,0,0
1,2,0,100,0,0,0,468,0,61302,2,9,65535,2,0,0
1,2,0,100,0,0,0,1340,0,61301,3,9,2,3,0,1
0,2,0,100,0,0,0,900,0,69664,4,9,0,4,0,1
CSV

cat > "$WORK/mlx5_devx_qp_connect.csv" <<CSV
$T,dest_gid_prefix,dest_gid_iface,src_gid_prefix,src_gid_iface,qp_num,qp_obj,dest_qp_num,dest_lid,src_lid,port_num
1,1,0,200,0,0,0,0,0,0,0,61301,467,61302,7837,7837,1
1,1,0,200,0,0,0,0,0,0,0,61302,468,61301,7837,7837,1
1,1,0,210,0,0,0,0,0,0,0,61301,1340,69664,7820,7821,1
0,1,0,210,0,0,0,0,0,0,0,69664,900,61301,7821,7820,1
CSV

S=laddr,raddr,qp_num,qp_obj,wqe_idx,lkey,rkey,length,dest_dct,opcode,decode,av_ds,submitter,dest_lid,pad,dest_gid_prefix,dest_gid_iface
cat > "$WORK/mlx5_device_send.csv" <<CSV
$T,$S
1,1,0,1000,0,0,0,0,0,61301,1340,0,0,0,10,0,8,0,0,1,0,0,0,0
1,1,0,1010,0,0,0,0,0,61301,1340,1,0,0,10,0,8,0,0,1,0,0,0,0
1,1,0,1020,0,0,0,0,0,61301,1340,2,0,0,10,0,8,0,0,1,0,0,0,0
1,1,0,1030,0,0,0,0,0,61301,467,0,0,0,5,0,8,0,0,3,0,0,0,0
1,1,0,1040,0,0,0,0,0,61301,0,3,0,0,7,0,8,0,0,1,0,0,0,0
0,1,0,1050,0,0,0,0,0,69664,0,0,0,0,11,0,8,0,0,1,0,0,0,0
CSV

"$COMSHU" link "$WORK" > "$WORK/link.out" 2>&1
link_rc=$?
if grep -q "no DuckDB" "$WORK/link.out"; then echo "SKIP: build without DuckDB"; exit 0; fi
if [ "$link_rc" -ne 0 ] || grep -qE "SOME FAILED| failed: " "$WORK/link.out"; then
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

expect "each send reaches its own queue pair's peer; an ambiguous number reaches no one" \
    "SELECT string_agg(src_rank||'>'||dst_rank||':'||sends||':'||bytes, ' ' ORDER BY src_rank, dst_rank) FROM comm_edges WHERE transport = 'devx'" \
    "0>1:1:11 1>-1:1:7 1>0:3:30 1>1:1:5"

expect "a queue pair is (rank, qp_num, qp_obj): the two 61301s are two endpoints" \
    "SELECT string_agg(qp_obj||':'||coalesce(peer_rank::VARCHAR,'?')||':'||sends, ' ' ORDER BY qp_obj) FROM endpoint WHERE rank = 1 AND qp_num = 61301" \
    "467:1:1 1340:0:3"

expect "the unresolved send is counted, not dropped" \
    "SELECT n FROM link_health WHERE finding = 'unresolved_sends'" "1"

[ "$fail" -eq 0 ] || exit 1
echo "ok: same-numbered queue pairs on one rank stay apart"
