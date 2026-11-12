# wr_api_ops — the rest of the ibv_wr_* API, on a NIC

Evidence for the `ibv_qp_ex` wrappers in `providers/ibverbs/ibverbs_provider.cpp`
(atomics, `*_imm`, `send_inv`, `send_tso`, `local_inv`, `bind_mw`, `flush`,
`atomic_write`, inline data). `tests/wr_builder_test` covers the bookkeeping
without hardware; this proves the provider calls the pointers the way the
wrappers assume. Nothing in the corpus uses these calls: NCCL 2.32.6, NVSHMEM
and DeepEP all post with classic `ibv_post_send`.

    gcc -O2 -o wr_api_ops wr_api_ops.c -libverbs
    COMSHU_ROOT=... ; source tests/hpc/suite/common.sh; comshu_load_runtime
    $COMSHU run -p ib -o out -- ./wr_api_ops
    $COMSHU export out --csv out.csv && cat out.csv/ib_post_send.csv

One process, two RC queue pairs on port 1 connected to each other, and one
batch of four work requests. Exit 0 only if every completion succeeded and B
holds the expected bytes.

## du04, 2026-11-12 (mlx5_0, IB)

    wr_api_ops: OK on mlx5_0 (IB) — fetch_add 37->42, write_imm 32 B inline,
                send 40 B inline from 2 buffers, write 128 B

    wr_id,remote_addr,rkey,length,send_flags,opcode,num_sge
    1,0x…080,1940828,8,signaled,atomic_fetch_add,1
    2,0x…0c0,1940828,32,signaled|inline,rdma_write_imm,1
    3,0x0,0,40,signaled|inline,send,2
    4,0x…180,1940828,128,signaled,rdma_write,1

Before the wrappers, rows 1 and 2 were not recorded at all. Worse, the
inline payload of 2 had nowhere to go but the still-open row before it.
