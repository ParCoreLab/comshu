/* A deterministic UCX workload for the record gate, over the SELF transport.
 *
 * The UCX provider had no test of any kind: everything it recorded was verified
 * by reading MN5 job output by hand, which needs an allocation, a queue wait and
 * a two-node reservation to learn that a record is missing. UCX does not need
 * any of that to run — the self/shm transports work in one process on a laptop —
 * so this exercises the doors locally and tests/ucx_records.sh asserts the rows.
 *
 * It is deliberately NOT a benchmark and NOT a realistic workload. Every
 * operation here exists because some record depends on it, and the counts are
 * fixed so the assertions can be exact rather than "greater than zero":
 *
 *   1 x ucp_tag_send_nbx      the modern door
 *   1 x ucp_tag_send_nb       the legacy door, which UCX implements over the
 *                             modern one — this is the case that caught the
 *                             double-count (see legacy_depth in the provider)
 *   1 x ucp_tag_send_sync_nb  MPI_Ssend's door, which must stay distinguishable
 *   1 x ucp_tag_recv_nbx / ucp_tag_recv_nb
 *   1 x ucp_tag_probe_nb + ucp_tag_msg_recv_nb
 *                             the pair whose join is the only way a msg_recv
 *                             row recovers its tag
 *   1 x ucp_mem_map / ucp_mem_unmap
 *
 * Self-transport tag matching means a send and its receive are the same rank,
 * which is fine: this asserts what the PROVIDER recorded, not what the network
 * did.
 *
 * C++ rather than C only because the project enables no C language; the code is
 * plain C and the UCX callbacks keep C linkage so their function pointers are
 * what UCX expects.
 */
#include <ucp/api/ucp.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static ucp_worker_h g_worker;

extern "C" void send_cb(void* request, ucs_status_t status) {
    (void)request;
    (void)status;
}
extern "C" void recv_cb(void* request, ucs_status_t status, ucp_tag_recv_info_t* info) {
    (void)request;
    (void)status;
    (void)info;
}

/* Drive progress until the request retires. A NULL request completed inline and
 * an error pointer never will, so neither is waited on. */
static int settle(void* request) {
    if (request == NULL)
        return 0;
    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "ucx_selftest: request failed: %s\n",
                ucs_status_string(UCS_PTR_STATUS(request)));
        return 1;
    }
    while (ucp_request_check_status(request) == UCS_INPROGRESS)
        ucp_worker_progress(g_worker);
    ucp_request_free(request);
    return 0;
}

int main(void) {
    ucp_config_t* config;
    ucp_context_h context;
    ucp_params_t  params;
    int           rc = 0;

    memset(&params, 0, sizeof params);
    params.field_mask = UCP_PARAM_FIELD_FEATURES;
    params.features = UCP_FEATURE_TAG | UCP_FEATURE_RMA | UCP_FEATURE_AMO64;

    if (ucp_config_read(NULL, NULL, &config) != UCS_OK) {
        fprintf(stderr, "ucx_selftest: ucp_config_read failed\n");
        return 77; /* treated as a skip by the runner */
    }
    if (ucp_init(&params, config, &context) != UCS_OK) {
        ucp_config_release(config);
        fprintf(stderr, "ucx_selftest: ucp_init failed — no usable transport\n");
        return 77;
    }
    ucp_config_release(config);

    ucp_worker_params_t wparams;
    memset(&wparams, 0, sizeof wparams);
    wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    wparams.thread_mode = UCS_THREAD_MODE_SINGLE;
    if (ucp_worker_create(context, &wparams, &g_worker) != UCS_OK) {
        fprintf(stderr, "ucx_selftest: ucp_worker_create failed\n");
        ucp_cleanup(context);
        return 77;
    }

    /* BOTH SPELLINGS OF "give me this worker's address", because a provider that
     * hooks one of them looks identical to a provider that hooks both until an
     * application picks the other. That is not hypothetical: comshu hooked only
     * ucp_worker_get_address, OpenMPI's point-to-point path (mca_pml_ucx) uses
     * only ucp_worker_query, and three of the four studies in tests/hpc/apps/
     * independently found that ucx_worker_address was therefore describing the
     * wrong worker for every MPI application. This gate could not see it,
     * because this file called the same door the provider happened to hook.
     *
     * The query goes first and asks for the address explicitly. UCP_WORKER_ATTR_
     * FIELD_ADDRESS is the whole point: ucp_worker_query fills only what the
     * caller asks for, so a provider reading attr->address without checking the
     * mask would hash uninitialised stack. Zeroing attr here would hide that,
     * hence the deliberate garbage fill below. */
    ucp_worker_attr_t wattr;
    memset(&wattr, 0xA5, sizeof wattr); /* poison: nothing may be read unasked */
    wattr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;
    if (ucp_worker_query(g_worker, &wattr) != UCS_OK) {
        fprintf(stderr, "ucx_selftest: ucp_worker_query failed\n");
        ucp_worker_destroy(g_worker);
        ucp_cleanup(context);
        return 77;
    }
    ucp_worker_release_address(g_worker, wattr.address);

    /* A second query that does NOT ask for the address. A provider that ignores
     * the field mask emits a row here, hashing the poison above; one that
     * respects it emits nothing. The row count downstream is what tells them
     * apart. */
    ucp_worker_attr_t nattr;
    memset(&nattr, 0xA5, sizeof nattr);
    nattr.field_mask = UCP_WORKER_ATTR_FIELD_THREAD_MODE;
    (void)ucp_worker_query(g_worker, &nattr);

    /* An endpoint to our own worker: the self transport, no network needed. */
    ucp_address_t* address;
    size_t         address_len;
    if (ucp_worker_get_address(g_worker, &address, &address_len) != UCS_OK) {
        fprintf(stderr, "ucx_selftest: ucp_worker_get_address failed\n");
        ucp_worker_destroy(g_worker);
        ucp_cleanup(context);
        return 77;
    }
    ucp_ep_params_t eparams;
    memset(&eparams, 0, sizeof eparams);
    eparams.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    eparams.address = address;
    ucp_ep_h     ep;
    ucs_status_t st = ucp_ep_create(g_worker, &eparams, &ep);
    ucp_worker_release_address(g_worker, address);
    if (st != UCS_OK) {
        fprintf(stderr, "ucx_selftest: ucp_ep_create failed: %s\n", ucs_status_string(st));
        ucp_worker_destroy(g_worker);
        ucp_cleanup(context);
        return 77;
    }

    char send_buf[128], recv_buf[128];
    memset(send_buf, 'x', sizeof send_buf);

    ucp_request_param_t rparams;
    memset(&rparams, 0, sizeof rparams);
    const ucp_datatype_t contig = ucp_dt_make_contig(1);

    /* 1. the modern door, 32 bytes, tag 0x1234 */
    rc |= settle(ucp_tag_send_nbx(ep, send_buf, 32, 0x1234, &rparams));
    rc |= settle(ucp_tag_recv_nbx(g_worker, recv_buf, 32, 0x1234, (ucp_tag_t)-1, &rparams));

    /* 2. the legacy door, 16 bytes, tag 0x5678 */
    rc |= settle(ucp_tag_send_nb(ep, send_buf, 16, contig, 0x5678, send_cb));
    rc |= settle(ucp_tag_recv_nb(g_worker, recv_buf, 16, contig, 0x5678, (ucp_tag_t)-1, recv_cb));

    /* 3. the synchronous door, 24 bytes, tag 0x9999. Its receive is posted
     *    first: a sync send does not complete until the receiver matches, so
     *    sending into an unposted queue would spin forever. */
    void* sync_recv =
        ucp_tag_recv_nb(g_worker, recv_buf, 24, contig, 0x9999, (ucp_tag_t)-1, recv_cb);
    rc |= settle(ucp_tag_send_sync_nb(ep, send_buf, 24, contig, 0x9999, send_cb));
    rc |= settle(sync_recv);

    /* 4. probe + msg_recv, 48 bytes, tag 0xABCD. The probe is where this
     *    receive's tag comes from; ucp_tag_msg_recv_nb itself carries none. */
    void*               probed_send = ucp_tag_send_nb(ep, send_buf, 48, contig, 0xABCD, send_cb);
    ucp_tag_recv_info_t info;
    ucp_tag_message_h   message = NULL;
    memset(&info, 0, sizeof info);
    for (int i = 0; i < 100000 && message == NULL; ++i) {
        message = ucp_tag_probe_nb(g_worker, 0xABCD, (ucp_tag_t)-1, 1 /* remove */, &info);
        if (message == NULL)
            ucp_worker_progress(g_worker);
    }
    if (message == NULL) {
        fprintf(stderr, "ucx_selftest: probe never matched — cannot exercise msg_recv\n");
        rc = 1;
    } else {
        rc |= settle(ucp_tag_msg_recv_nb(g_worker, recv_buf, 48, contig, message, recv_cb));
    }
    rc |= settle(probed_send);

    /* 5. a registration, so ucx_mem_map has a row and its close is balanced. */
    ucp_mem_map_params_t mparams;
    memset(&mparams, 0, sizeof mparams);
    mparams.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    mparams.address = send_buf;
    mparams.length = sizeof send_buf;
    ucp_mem_h memh;
    if (ucp_mem_map(context, &mparams, &memh) == UCS_OK)
        ucp_mem_unmap(context, memh);

    /* 6. ONE-SIDED, and it is here for the UCT layer rather than for UCP.
     *    ucx_rma already had rows on MN5; what nothing local had ever exercised
     *    is the put/get slots of uct_iface_ops_t, which the `self` transport
     *    fills (uct_sm_ep_put_short / uct_sm_ep_get_bcopy). It also gives
     *    tests/ucx_deep_records.sh the one EXACT cross-layer assertion available
     *    in a single process: a small put over a loopback endpoint must reach
     *    uct_send.remote_addr carrying the very address ucp_put_nbx was handed.
     *
     *    Deliberately small (8 bytes) so UCX picks the short protocol and does
     *    not fragment; a fragmented put would be several uct_sends at several
     *    offsets and the address assertion would have to become a range test. */
    static char rma_target[64];
    memset(rma_target, 0, sizeof rma_target);
    ucp_mem_map_params_t rparams_mm;
    memset(&rparams_mm, 0, sizeof rparams_mm);
    rparams_mm.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    rparams_mm.address = rma_target;
    rparams_mm.length = sizeof rma_target;
    ucp_mem_h rma_memh;
    if (ucp_mem_map(context, &rparams_mm, &rma_memh) == UCS_OK) {
        void*  rkey_buf = NULL;
        size_t rkey_len = 0;
        if (ucp_rkey_pack(context, rma_memh, &rkey_buf, &rkey_len) == UCS_OK) {
            ucp_rkey_h rkey;
            if (ucp_ep_rkey_unpack(ep, rkey_buf, &rkey) == UCS_OK) {
                /* The address is printed so the gate can assert on it rather
                 * than on a number it had to guess. */
                printf("ucx_selftest: rma_target %p\n", (void*)rma_target);
                rc |= settle(ucp_put_nbx(ep, send_buf, 8, (uint64_t)(uintptr_t)rma_target, rkey,
                                         &rparams));
                rc |= settle(ucp_get_nbx(ep, recv_buf, 8, (uint64_t)(uintptr_t)rma_target, rkey,
                                         &rparams));
                rc |= settle(ucp_ep_flush_nbx(ep, &rparams));
                ucp_rkey_destroy(rkey);
            }
            ucp_rkey_buffer_release(rkey_buf);
        }
        ucp_mem_unmap(context, rma_memh);
    }

    settle(ucp_ep_close_nb(ep, UCP_EP_CLOSE_MODE_FLUSH));
    ucp_worker_destroy(g_worker);
    ucp_cleanup(context);

    printf("ucx_selftest: %s\n", rc == 0 ? "workload ok" : "WORKLOAD FAILED");
    return rc;
}
