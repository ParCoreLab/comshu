#pragma once
// The state behind rdma-core's *new* work-request API, with no ibv_ type in it.
//
// `ibv_post_send` takes a linked list of fully-formed work requests, so one
// hooked call sees everything and the provider just walks the chain. The newer
// API is a builder spread across several calls on `struct ibv_qp_ex`:
//
//     ibv_wr_start(qpx);
//     qpx->wr_id = 7;
//     ibv_wr_send(qpx);                       // opcode, and a new WR begins
//     ibv_wr_set_sge(qpx, lkey, addr, 4096);  // ... its payload
//     qpx->wr_id = 8;
//     ibv_wr_rdma_write(qpx, rkey, raddr);    // another WR in the same batch
//     ibv_wr_set_sge_list(qpx, 2, sgl);
//     ibv_wr_complete(qpx);                   // the whole batch is posted here
//
// None of that is interceptable from outside: those are `static inline`
// wrappers in verbs.h that call through function pointers in the qp_ex struct,
// so LD_PRELOAD has nothing to bind. The provider patches the pointers instead,
// and every patched call lands here.
//
// This type holds no vendor types on purpose. The sequencing is where the bugs
// live — a WR that never gets an sge, an sge with no WR open, a batch abandoned
// by wr_abort, a wr_complete that fails — and none of that needs a NIC to test.
// providers/ibverbs is otherwise unbuildable without libibverbs, which on most
// developer machines means untestable.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace comshu::providers::ibverbs {

// ibv_wr_opcode values, restated so this header needs no verbs.h. Deliberately
// the *same* encoding the old ibv_post_send path records, so a capture never
// reveals which API the application used and every existing query still works.
enum class WrOpcode : std::uint8_t {
    RdmaWrite = 0,
    RdmaWriteWithImm = 1,
    Send = 2,
    SendWithImm = 3,
    RdmaRead = 4,
    AtomicCmpSwp = 5,
    AtomicFetchAdd = 6,
    LocalInv = 7,
    BindMw = 8,
    SendWithInv = 9,
    Tso = 10,
    Flush = 14,
    AtomicWrite = 15,
};

// One completed work request, in the shape IbPostSend wants.
struct PendingWr {
    std::uint64_t wr_id{0};
    std::uint64_t remote_addr{0};  // RDMA, atomic and flush opcodes only
    std::uint32_t rkey{0};         // RDMA, atomic and flush opcodes only
    std::uint32_t length{0};       // total sg_list bytes
    // ibv_qp_ex::wr_flags at the moment the opcode call was made. The caller
    // assigns it there before calling ibv_wr_send/rdma_write/etc, exactly as it
    // assigns wr_id, so it is read on the same call and for the same reason.
    std::uint32_t send_flags{0};
    std::uint8_t  opcode{0};
    std::uint8_t  num_sge{0};
};

class WrBuilder {
public:
    // ibv_wr_start: a new batch. Anything still pending belonged to a batch that
    // was never completed or aborted — the application leaked it, and keeping it
    // would attribute those sends to this batch's timestamps.
    void start() {
        m_pending.clear();
        m_open = false;
        m_started = true;
    }

    // Any of the wr_<opcode> calls: closes the previous WR and opens a new one.
    // `wr_id` is read from the qp_ex struct by the caller, since the application
    // assigns it there before calling the opcode function.
    void begin(WrOpcode op, std::uint64_t wr_id, std::uint32_t send_flags) {
        if (!m_started)
            return;  // opcode outside a batch: not ours to guess at
        m_pending.push_back(
            PendingWr{wr_id, 0, 0, 0, send_flags, static_cast<std::uint8_t>(op), 0});
        m_open = true;
    }

    // wr_rdma_write / wr_rdma_read / the atomics / wr_flush carry the remote
    // address on the same call that sets the opcode, so the provider passes it
    // straight through.
    void set_rdma(std::uint32_t rkey, std::uint64_t remote_addr) {
        if (!m_open)
            return;
        m_pending.back().rkey = rkey;
        m_pending.back().remote_addr = remote_addr;
    }

    // ibv_wr_set_sge: exactly one scatter-gather entry, replacing any previous.
    void set_sge(std::uint32_t length) {
        if (!m_open)
            return;
        m_pending.back().length = length;
        m_pending.back().num_sge = 1;
    }

    // ibv_wr_set_sge_list: n entries; only the total matters to the record.
    void set_sge_list(std::size_t num_sge, const std::uint32_t* lengths) {
        if (!m_open)
            return;
        std::uint32_t total = 0;
        for (std::size_t i = 0; i < num_sge; ++i)
            total += lengths[i];
        m_pending.back().length = total;
        m_pending.back().num_sge = static_cast<std::uint8_t>(num_sge > 255 ? 255 : num_sge);
    }

    // ibv_wr_set_inline_data[_list]: the payload is copied into the WQE rather
    // than named by an sge, but it is still the work request's payload, so it is
    // recorded the way the old path records an IBV_SEND_INLINE chain: total
    // bytes, and one entry per buffer.
    void set_inline(std::size_t num_buf, std::uint64_t total) {
        if (!m_open)
            return;
        m_pending.back().length = static_cast<std::uint32_t>(total);
        m_pending.back().num_sge = static_cast<std::uint8_t>(num_buf > 255 ? 255 : num_buf);
    }

    // wr_atomic_write carries its 8-byte operand on the opcode call itself; no
    // sge follows, so its length is set here.
    void set_length(std::uint32_t length) {
        if (!m_open)
            return;
        m_pending.back().length = length;
    }

    // ibv_wr_complete: the batch is posted. Returns what to record, and resets.
    //
    // On failure nothing reached the wire, so nothing is recorded — the old path
    // makes the same call by returning early when ibv_post_send returns nonzero.
    // Recording them anyway would put sends in the capture that never happened,
    // which is worse than missing ones: a reader cannot tell it is looking at a
    // failure.
    std::vector<PendingWr> complete(bool ok) {
        std::vector<PendingWr> out;
        if (ok)
            out.swap(m_pending);
        reset();
        return out;
    }

    // ibv_wr_abort: the batch is discarded, so it never happened.
    void abort() { reset(); }

    // Only for tests and diagnostics.
    std::size_t pending() const { return m_pending.size(); }
    bool        in_batch() const { return m_started; }

private:
    void reset() {
        m_pending.clear();
        m_open = false;
        m_started = false;
    }

    std::vector<PendingWr> m_pending;
    bool                   m_open{false};     // a wr_<opcode> call has opened a WR
    bool                   m_started{false};  // between wr_start and wr_complete/wr_abort
};

}  // namespace comshu::providers::ibverbs
