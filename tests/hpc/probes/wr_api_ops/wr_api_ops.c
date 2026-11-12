// wr_api_ops — every payload-carrying shape of rdma-core's ibv_wr_* API, on real
// hardware, in one process: two RC queue pairs on one port, connected to each
// other, one batch from QP A to QP B.
//
// What it exists to prove: providers/ibverbs patches each ibv_qp_ex function
// pointer and rebuilds the batch into ib_post_send rows. tests/wr_builder_test
// covers the bookkeeping; only a NIC proves that the provider calls the
// pointers the way the wrappers assume. Nothing in the corpus uses these calls
// (NCCL, NVSHMEM and DeepEP post with classic ibv_post_send), so this is where
// they are exercised.
//
// The batch, wr_id = expected row:
//   1  atomic_fetch_add  B's word += 5, old value into A's result buffer (sge 8)
//   2  rdma_write_imm    32 B inline into B's buffer at +64, imm 0xC0FFEE
//   3  send              inline from two buffers, 16 + 24 = 40 B
//   4  rdma_write        sge of 128 B into B's buffer at +256
//
//   gcc -O2 -o wr_api_ops wr_api_ops.c -libverbs
//   comshu run -p ib -o out -- ./wr_api_ops && comshu export out --csv out.csv
//
// Exit 0 only if every completion succeeded AND the remote side holds the
// expected bytes, so a recorded batch is also a batch that happened.
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, "wr_api_ops: " __VA_ARGS__); fputc('\n', stderr); exit(2); } while (0)

static struct ibv_qp *make_qp(struct ibv_context *ctx, struct ibv_pd *pd, struct ibv_cq *cq) {
    struct ibv_qp_init_attr_ex a = {0};
    a.qp_type = IBV_QPT_RC;
    a.send_cq = cq;
    a.recv_cq = cq;
    a.cap.max_send_wr = 16;
    a.cap.max_recv_wr = 16;
    a.cap.max_send_sge = 2;
    a.cap.max_recv_sge = 1;
    a.cap.max_inline_data = 64;
    a.pd = pd;
    a.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
    a.send_ops_flags = IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_WRITE |
                       IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM | IBV_QP_EX_WITH_ATOMIC_FETCH_AND_ADD;
    struct ibv_qp *qp = ibv_create_qp_ex(ctx, &a);
    if (!qp) DIE("ibv_create_qp_ex failed (send_ops_flags unsupported?)");
    return qp;
}

static void connect_qp(struct ibv_qp *qp, uint32_t dest_qpn, uint16_t lid, union ibv_gid *gid,
                       int use_gid, int gid_index) {
    struct ibv_qp_attr a = {0};
    a.qp_state = IBV_QPS_INIT;
    a.port_num = 1;
    a.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        DIE("INIT");
    memset(&a, 0, sizeof a);
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = IBV_MTU_1024;
    a.dest_qp_num = dest_qpn;
    a.max_dest_rd_atomic = 1;
    a.min_rnr_timer = 12;
    a.ah_attr.dlid = lid;
    a.ah_attr.port_num = 1;
    if (use_gid) {
        a.ah_attr.is_global = 1;
        a.ah_attr.grh.dgid = *gid;
        a.ah_attr.grh.sgid_index = gid_index;
        a.ah_attr.grh.hop_limit = 1;
    }
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                                  IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
        DIE("RTR");
    memset(&a, 0, sizeof a);
    a.qp_state = IBV_QPS_RTS;
    a.timeout = 14;
    a.retry_cnt = 7;
    a.rnr_retry = 7;
    a.max_rd_atomic = 1;
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                  IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
        DIE("RTS");
}

int main(void) {
    int n = 0;
    struct ibv_device **devs = ibv_get_device_list(&n);
    if (!devs || n == 0) DIE("no RDMA device");
    const char *want = getenv("WR_API_DEV");
    struct ibv_device *dev = devs[0];
    for (int i = 0; want && i < n; ++i)
        if (!strcmp(ibv_get_device_name(devs[i]), want)) dev = devs[i];
    struct ibv_context *ctx = ibv_open_device(dev);
    if (!ctx) DIE("open %s", ibv_get_device_name(dev));
    struct ibv_port_attr port;
    if (ibv_query_port(ctx, 1, &port) || port.state != IBV_PORT_ACTIVE) DIE("port 1 not active");
    const int use_gid = port.link_layer == IBV_LINK_LAYER_ETHERNET;
    const int gid_index = use_gid ? 3 : 0;  // RoCE v2 IPv4 GID is conventionally index 3
    union ibv_gid gid;
    memset(&gid, 0, sizeof gid);
    if (use_gid && ibv_query_gid(ctx, 1, gid_index, &gid)) DIE("query_gid");

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_cq *cq = ibv_create_cq(ctx, 64, NULL, NULL, 0);
    static uint8_t a_buf[4096] __attribute__((aligned(64)));
    static uint8_t b_buf[4096] __attribute__((aligned(64)));
    const int acc = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_ATOMIC;
    struct ibv_mr *a_mr = ibv_reg_mr(pd, a_buf, sizeof a_buf, acc);
    struct ibv_mr *b_mr = ibv_reg_mr(pd, b_buf, sizeof b_buf, acc);
    if (!pd || !cq || !a_mr || !b_mr) DIE("pd/cq/mr");

    struct ibv_qp *qa = make_qp(ctx, pd, cq), *qb = make_qp(ctx, pd, cq);
    connect_qp(qa, qb->qp_num, port.lid, &gid, use_gid, gid_index);
    connect_qp(qb, qa->qp_num, port.lid, &gid, use_gid, gid_index);

    // B pre-posts two receives: one for the write-with-immediate, one for the send.
    for (int i = 0; i < 2; ++i) {
        struct ibv_sge s = {(uintptr_t)(b_buf + 1024 + 128 * i), 128, b_mr->lkey};
        struct ibv_recv_wr r = {.wr_id = 100 + i, .sg_list = &s, .num_sge = 1}, *bad;
        if (ibv_post_recv(qb, &r, &bad)) DIE("post_recv");
    }

    uint64_t *word = (uint64_t *)b_buf;
    *word = 37;
    char inl_a[16], inl_b[24], inl_w[32];
    memset(inl_a, 'a', sizeof inl_a);
    memset(inl_b, 'b', sizeof inl_b);
    memset(inl_w, 'w', sizeof inl_w);
    memset(a_buf + 512, 'x', 128);
    const uint64_t rb = (uintptr_t)b_buf;

    struct ibv_qp_ex *x = ibv_qp_to_qp_ex(qa);
    if (!x) DIE("ibv_qp_to_qp_ex");
    ibv_wr_start(x);
    x->wr_flags = IBV_SEND_SIGNALED;
    x->wr_id = 1;
    ibv_wr_atomic_fetch_add(x, b_mr->rkey, rb, 5);
    ibv_wr_set_sge(x, a_mr->lkey, (uintptr_t)a_buf, 8);
    x->wr_id = 2;
    x->wr_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    ibv_wr_rdma_write_imm(x, b_mr->rkey, rb + 64, htonl(0xC0FFEE));
    ibv_wr_set_inline_data(x, inl_w, sizeof inl_w);
    x->wr_id = 3;
    ibv_wr_send(x);
    struct ibv_data_buf bufs[2] = {{inl_a, sizeof inl_a}, {inl_b, sizeof inl_b}};
    ibv_wr_set_inline_data_list(x, 2, bufs);
    x->wr_id = 4;
    x->wr_flags = IBV_SEND_SIGNALED;
    ibv_wr_rdma_write(x, b_mr->rkey, rb + 256);
    ibv_wr_set_sge(x, a_mr->lkey, (uintptr_t)(a_buf + 512), 128);
    if (ibv_wr_complete(x)) DIE("ibv_wr_complete");

    // 4 send completions + 2 receive completions on B.
    int got = 0, bad_status = 0;
    for (long spins = 0; got < 6 && spins < 100000000L; ++spins) {
        struct ibv_wc wc[8];
        int k = ibv_poll_cq(cq, 8, wc);
        for (int i = 0; i < k; ++i) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "wr_api_ops: wr_id %lu status %s\n", (unsigned long)wc[i].wr_id,
                        ibv_wc_status_str(wc[i].status));
                bad_status = 1;
            }
        }
        got += k > 0 ? k : 0;
    }
    if (got < 6 || bad_status) DIE("completions: %d of 6%s", got, bad_status ? ", some failed" : "");

    int ok = *word == 42 && *(uint64_t *)a_buf == 37 && b_buf[64] == 'w' && b_buf[95] == 'w' &&
             b_buf[256] == 'x' && b_buf[383] == 'x' && b_buf[1024 + 128] == 'a' &&
             b_buf[1024 + 128 + 16] == 'b' && b_buf[1024 + 128 + 39] == 'b';
    printf("wr_api_ops: %s on %s (%s) — fetch_add 37->%lu, write_imm 32 B inline, send 40 B inline "
           "from 2 buffers, write 128 B\n",
           ok ? "OK" : "DATA MISMATCH", ibv_get_device_name(dev), use_gid ? "RoCE" : "IB",
           (unsigned long)*word);
    ibv_destroy_qp(qa);
    ibv_destroy_qp(qb);
    ibv_dereg_mr(a_mr);
    ibv_dereg_mr(b_mr);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(devs);
    return ok ? 0 : 1;
}
