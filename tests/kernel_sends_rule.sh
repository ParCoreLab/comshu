#!/usr/bin/env bash
# kernel_sends: which kernel issued each GPU-initiated send.
#
# The rule is a window join — a send belongs to every kernel on its queue's GPU
# whose execution window contains the doorbell — and MN5 job 48813620 cannot
# test most of it: NCCL GIN's put-ring ran one kernel at a time on one GPU per
# rank, and every doorbell fell inside exactly one put_ring. So the fixture
# builds the cases that capture never had, each with something to get wrong:
#
#   K1 GPU 0 [1000,2000]   the plain case
#   K2 GPU 1 [1000,2000]   same window, OTHER GPU: a queue on GPU 0 must not
#                          credit it; a queue whose GPU is unknown must
#   K3 GPU 0 [1800,3000]   overlaps K1: a doorbell at 1900 is in BOTH, counted
#                          in both and as shared in both — never split by guess
#   K4 GPU 0 [1000,5000]   source = 2, the launch CALL not the execution: must
#                          not be a window at all, or it swallows the doorbell
#                          at 4000 that no kernel was running for
#
# Sends (one clock: domain 0 for the doorbells, domain 1 for the kernels, both
# anchored 1:1 so the alignment is exercised but the numbers stay readable):
#   qp 7 (GPU 0)   1500 -> K1;  1900 -> K1+K3;  2500 -> K3;  4000 -> none
#   qp 9 (GPU ?)   1500 -> K1+K2
set -uo pipefail

PCOMM=${1:?usage: kernel_sends_rule.sh <pcomm-binary>}
if [ ! -x "$PCOMM" ]; then echo "SKIP: no pcomm binary at $PCOMM"; exit 0; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-kernel-sends-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/clock_anchor.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,domain,local_ticks,wall_ns,mono_ns
0,0,0,0,0,0,0,0,0,0,0
0,0,0,0,0,0,0,0,10000,10000,0
0,0,0,0,0,0,0,1,0,0,0
0,0,0,0,0,0,0,1,10000,10000,0
CSV

cat > "$WORK/gpu_kernel.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,correlation,stream,name,grid_x,grid_y,grid_z,device,block_x,block_y,block_z,source
0,2,1,1000,1,2000,0,1,1,1,1,1,1,0,1,1,1,0
0,2,1,1000,1,2000,0,2,1,2,1,1,1,1,1,1,1,0
0,2,1,1800,1,3000,0,3,2,3,1,1,1,0,1,1,1,0
0,2,1,1000,1,5000,0,4,1,4,1,1,1,0,1,1,1,2
CSV

cat > "$WORK/intern_text.csv" <<'CSV'
rank,domain,handle,text
0,3,1,k1
0,3,2,k2
0,3,3,k3
0,3,4,k4
CSV

cat > "$WORK/mlx5_devx_qp.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,obj,sq_base,qp_num,wq_umem_id,log_sq_size,device,cqn_snd,service_type,gpu_driven
0,2,0,100,0,900,0,1,0,7,1,9,0,1,0,1
0,2,0,100,0,900,0,2,0,9,2,9,65535,2,0,1
CSV

cat > "$WORK/mlx5_device_send.csv" <<'CSV'
rank,temporal_kind,temporal_a_domain,temporal_a_ticks,temporal_b_domain,temporal_b_ticks,temporal_value,laddr,raddr,qp_num,wqe_idx,lkey,rkey,length,dest_dct,opcode,decode,av_ds,submitter
0,1,0,1500,0,0,0,1,4096,7,0,1,2,10,0,8,0,0,2
0,1,0,1900,0,0,0,1,4096,7,1,1,2,20,0,8,0,0,2
0,1,0,2500,0,0,0,1,4096,7,2,1,2,40,0,8,0,0,2
0,1,0,4000,0,0,0,1,4096,7,3,1,2,80,0,8,0,0,2
0,1,0,1500,0,0,0,1,4096,9,0,1,2,160,0,8,0,0,2
CSV

"$PCOMM" link "$WORK" > "$WORK/link.out" 2>&1
link_rc=$?
if grep -q "no DuckDB" "$WORK/link.out"; then echo "SKIP: build without DuckDB"; exit 0; fi
if [ "$link_rc" -ne 0 ] || grep -q "SOME FAILED" "$WORK/link.out"; then
    cat "$WORK/link.out"; echo "FAIL: link did not complete"; exit 1
fi

fail=0
expect() {  # <label> <sql> <expected>
    local got
    got=$("$PCOMM" sql "$WORK" "$2" 2>/dev/null | tail -1 | tr -d '"')
    if [ "$got" != "$3" ]; then
        echo "FAIL: $1"; echo "  expected: $3"; echo "  got:      ${got:-<no result>}"; echo "  query:    $2"
        fail=1
    fi
    return 0
}

# Every attribution in one value: kernel:sends:bytes:shared.
expect "each kernel holds exactly the doorbells inside its window on its GPU" \
    "SELECT string_agg(kernel||':'||sends||':'||bytes||':'||shared_sends, ' ' ORDER BY kernel) FROM kernel_sends" \
    "k1:3:190:2 k2:1:160:1 k3:2:60:1"

expect "the launch call is not an execution window" \
    "SELECT count(*) FROM kernel_sends WHERE kernel = 'k4'" "0"

expect "a doorbell after every kernel ended is counted, not credited" \
    "SELECT n FROM link_health WHERE finding = 'device_sends_outside_kernels'" "1"

[ "$fail" -eq 0 ] || exit 1
echo "ok: kernel_sends attributes by window, per GPU, and refuses to split overlaps"
