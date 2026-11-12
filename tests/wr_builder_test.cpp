// The new-WR-API state machine (providers/ibverbs/wr_builder.hpp).
//
// This is the piece that decides what `ib_post_send` rows an application using
// ibv_wr_start/ibv_wr_send/ibv_wr_complete produces. Before it existed those
// applications produced none at all: measured on MN5 job 43976008, ib_send_bw
// traced end to end gave 202,000 ib_poll_cq records and zero ib_post_send,
// because the wr_* calls are static inline wrappers over function pointers in
// ibv_qp_ex and never touch ctx->ops.post_send.
//
// The sequencing is where this can go wrong — a batch with several WRs, an sge
// list rather than a single sge, an aborted batch, a failed completion, a
// stray call outside a batch. None of that needs a NIC, and providers/ibverbs
// is otherwise unbuildable (and so untestable) without libibverbs, which is
// exactly why the state lives in a vendor-free header.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "wr_builder.hpp"

using namespace comshu::providers::ibverbs;

namespace {

int g_fail = 0;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok " : "XX ", what);
    if (!ok)
        g_fail = 1;
}

}  // namespace

int main() {
    std::printf("wr_builder: the new-WR-API batch state machine\n");

    // --- the ordinary case: one send, one sge -------------------------------
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 7, 2);
        b.set_sge(4096);
        auto out = b.complete(true);
        expect(out.size() == 1 && out[0].wr_id == 7 && out[0].length == 4096 &&
                   out[0].num_sge == 1 &&
                   out[0].opcode == static_cast<std::uint8_t>(WrOpcode::Send),
               "one send with one sge becomes one record");
        expect(out[0].rkey == 0 && out[0].remote_addr == 0,
               "and carries no remote address, because a send has none");
        // send_flags is read from ibv_qp_ex::wr_flags on the opcode call, the
        // same moment wr_id is. It decides whether this work request will
        // produce a completion at all, so losing it here would silently strip
        // the flight lane from every selectively signalling queue pair.
        expect(out[0].send_flags == 2, "and carries the send_flags it was opened with");
    }

    // --- several WRs in one batch, which is the whole point of the API ------
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 1, 0);
        b.set_sge(64);
        b.begin(WrOpcode::RdmaWrite, 2, 0);
        b.set_rdma(0xCAFEBABE, 0x1000);
        b.set_sge(128);
        b.begin(WrOpcode::RdmaRead, 3, 0);
        b.set_rdma(0xDEADBEEF, 0x2000);
        b.set_sge(256);
        auto out = b.complete(true);
        expect(out.size() == 3, "three work requests in one batch become three records");
        expect(out[0].wr_id == 1 && out[1].wr_id == 2 && out[2].wr_id == 3,
               "in order, each keeping its own wr_id");
        expect(out[1].rkey == 0xCAFEBABE && out[1].remote_addr == 0x1000 &&
                   out[2].rkey == 0xDEADBEEF && out[2].remote_addr == 0x2000,
               "and its own remote address");
        expect(out[0].rkey == 0 && out[0].remote_addr == 0,
               "while the send in the middle of them stays clean — the RDMA "
               "fields belong to the WR, not to the batch");
    }

    // --- sge lists ----------------------------------------------------------
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 1, 0);
        const std::uint32_t lens[] = {100, 200, 300};
        b.set_sge_list(3, lens);
        auto out = b.complete(true);
        expect(out.size() == 1 && out[0].length == 600 && out[0].num_sge == 3,
               "an sge list records the total bytes and the entry count");
    }
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 1, 0);
        b.set_sge(4096);
        const std::uint32_t lens[] = {1, 2};
        b.set_sge_list(2, lens);
        auto out = b.complete(true);
        expect(out.size() == 1 && out[0].length == 3 && out[0].num_sge == 2,
               "a later sge setter replaces the earlier one rather than adding "
               "to it — the API sets the payload, it does not append");
    }

    // --- a batch that never lands -------------------------------------------
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 1, 0);
        b.set_sge(64);
        auto out = b.complete(false);
        expect(out.empty(), "a failed wr_complete records nothing: those sends never reached "
                            "the wire, and inventing them is worse than missing them");
        expect(!b.in_batch(), "and the batch is closed either way");
    }
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 1, 0);
        b.set_sge(64);
        b.abort();
        expect(b.pending() == 0 && !b.in_batch(), "an aborted batch leaves nothing behind");
        // And the next batch is unaffected by it.
        b.start();
        b.begin(WrOpcode::Send, 2, 0);
        b.set_sge(8);
        auto out = b.complete(true);
        expect(out.size() == 1 && out[0].wr_id == 2,
               "the batch after an abort records only its own work requests");
    }

    // --- calls out of sequence, which a reader must never see as data -------
    {
        WrBuilder b;
        b.begin(WrOpcode::Send, 99, 0);  // no wr_start
        b.set_sge(64);
        auto out = b.complete(true);
        expect(out.empty(), "an opcode outside a batch is ignored rather than guessed at");
    }
    {
        WrBuilder b;
        b.start();
        b.set_sge(4096);  // no wr_<opcode> yet
        auto out = b.complete(true);
        expect(out.empty(), "an sge with no work request open is ignored");
    }
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 1, 0);  // no sge at all: a zero-length send
        auto out = b.complete(true);
        expect(out.size() == 1 && out[0].length == 0 && out[0].num_sge == 0,
               "a work request with no sge is still a work request — it is "
               "posted, so it is recorded, with the zero length it has");
    }

    // --- a batch left open by the application -------------------------------
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::Send, 1, 0);
        b.set_sge(64);
        b.start();  // second wr_start, first never completed
        b.begin(WrOpcode::Send, 2, 0);
        b.set_sge(128);
        auto out = b.complete(true);
        expect(out.size() == 1 && out[0].wr_id == 2,
               "a re-started batch drops the abandoned one instead of "
               "attributing its sends to this batch's timestamps");
    }

    // --- the rest of ibv_qp_ex: atomics, inline payloads, atomic write ------
    // The provider wraps every opcode function, and each lands here as begin()
    // plus whatever its call carries. Values are the ibv_wr_opcode encoding the
    // old path records, so a reader cannot tell the two APIs apart.
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::AtomicFetchAdd, 1, 0);
        b.set_rdma(0x77, 0x2000);  // wr_atomic_fetch_add's rkey and remote_addr
        b.set_sge(8);              // the local result buffer
        b.begin(WrOpcode::Send, 2, 0);
        b.set_inline(1, 40);  // wr_set_inline_data
        b.begin(WrOpcode::RdmaWrite, 3, 0);
        b.set_rdma(0x78, 0x3000);
        b.set_inline(3, 96);  // wr_set_inline_data_list, three buffers
        b.begin(WrOpcode::AtomicWrite, 4, 0);
        b.set_rdma(0x79, 0x4000);
        b.set_length(8);  // the operand rides on the opcode call
        auto out = b.complete(true);
        expect(out.size() == 4, "four work requests through four kinds of call");
        expect(out[0].opcode == 6 && out[0].rkey == 0x77 && out[0].remote_addr == 0x2000 &&
                   out[0].length == 8,
               "an atomic records its target and its 8-byte operand width");
        expect(out[1].length == 40 && out[1].num_sge == 1,
               "inline data is the work request's payload: 40 B in one buffer");
        expect(out[2].length == 96 && out[2].num_sge == 3 && out[2].rkey == 0x78,
               "an inline list sums its buffers and counts them");
        expect(out[3].opcode == 15 && out[3].length == 8 && out[3].remote_addr == 0x4000,
               "an atomic write carries its target and 8 bytes with no sge");
    }

    // --- why every opcode function is wrapped, payload or not ---------------
    // A wr_local_inv (or bind_mw, flush, ...) that the provider did NOT see
    // would leave the previous work request open, and the sge meant for the
    // next one would overwrite its length. Seeing the opcode is what closes it.
    {
        WrBuilder b;
        b.start();
        b.begin(WrOpcode::RdmaWrite, 1, 0);
        b.set_sge(4096);
        b.begin(WrOpcode::LocalInv, 2, 0);  // no payload
        b.begin(WrOpcode::Send, 3, 0);
        b.set_sge(16);
        auto out = b.complete(true);
        expect(out.size() == 3 && out[0].length == 4096 && out[1].length == 0 &&
                   out[2].length == 16,
               "a payload-less opcode closes the previous work request, so the next "
               "sge cannot rewrite its length");
    }

    std::printf("%s\n", g_fail ? "FAIL" : "PASS — the WR batch state machine holds");
    return g_fail;
}
