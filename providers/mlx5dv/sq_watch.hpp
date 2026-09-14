#pragma once
// The passive send-queue observer: the data plane of a DEVX queue pair whose
// send queue and doorbell record are both host memory — NVSHMEM's ibdevx
// transport above all. tests/hpc/cases/11_ibdevx_peermem/parity/README.md
// measured the gap this closes: under ibdevx neither pcomm nor SigmaTrace
// recorded one byte of the 16,384 the workload moved through the NIC.
//
// PROTOTYPE, opt-in via PCOMM_SQ_WATCH=1, to measure whether the design holds
// before it is given a launcher flag, a record of its own, and a case.
//
// WHY PASSIVE, rather than extending the host doorbell's UAR swap to BlueFlame
// UARs. ibdevx posts the way every mlx5 stack does (ibdevx.cpp,
// nvshmemt_ibdevx_post_send — the doorbell half identical in NVSHMEM v3.4.5,
// 3.7.2 and git):
//
//     STORE_BARRIER(); dbr->send_counter = htobe16(wqe_bb_idx); STORE_BARRIER();
//     memcpy(bf_reg + bf_reg_offset, wqe, 8);
//
// The WQE goes into the send queue, the new producer index into the doorbell
// record, and only then do eight bytes go to the UAR. The first two are plain
// host memory the NIC reads by DMA, so reading them sees every WQE the NIC is
// told about — from outside the path that tells it. Nothing is swapped, nothing
// is forwarded, and no stack can be broken by being watched. The UAR swap cannot
// offer that for a BF UAR: BF is also what UCX rings, and forwarding UCX's
// BlueFlame writes wrongly is how the host doorbell once hung MPI
// (things-to-circle-back-to.md, FIXED 2026-09-08). Nor does this need to tell
// NVSHMEM's UARs from UCX's, or to ask whether the platform maps UARs into the
// GPU (host_doorbell.hpp's guard): it never touches a UAR at all.
//
// What it costs, stated rather than discovered later:
//   * a row's time is when the watcher SAW the WQE, not when it was posted;
//   * one thread polling every watched doorbell record, and by default it SPINS,
//     which costs a core. Measured on du04 (case 11, 1024-slot rings): sleeping
//     20 us between polls — about 60 us once the kernel rounds it — let the CPU
//     proxy lap the whole ring, and one run decoded 9 of ~4,100 WQEs.
//     PCOMM_SQ_WATCH_US=<n> sleeps instead, and the lapped counter says what
//     that cost;
//   * a window of one ring. A producer that laps the ring between two polls has
//     overwritten WQEs nobody read; they are counted as lapped, never decoded
//     from whatever bytes replaced them.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <endian.h>

#include "uar_hijack.hpp"

namespace mlx5::sqwatch {

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_SQ_WATCH");
        return v && *v == '1';
    }();
    return on;
}

inline bool debug_on() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_SQ_WATCH_DEBUG");
        return v && *v == '1';
    }();
    return on;
}

// Zero, the default, means spin.
inline std::chrono::microseconds poll_interval() {
    static const auto us = [] {
        const char* v = std::getenv("PCOMM_SQ_WATCH_US");
        const long  n = v ? std::strtol(v, nullptr, 10) : 0;
        return std::chrono::microseconds(n > 0 ? n : 0);
    }();
    return us;
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

// The mlx5 doorbell record is two big-endian 32-bit counters: receive at offset
// 0, send at offset 4 (rdma-core's qp->db[MLX5_SND_DBR]). ibdevx declares it as
// four be16 and writes the send counter at offset 6 — the low half of the same
// field, with the high half never written. WQE indices wrap at 2^16, so only the
// low half carries meaning and the high half is masked off rather than trusted.
inline constexpr std::size_t kSndDbrOffset = 4;

// WQEBBs a producer may have written but not yet published in the doorbell
// record. The copy is discarded if the producer could have reached the slots it
// covers; two is the widest WQE ibdevx builds (a 64-bit AMO, one WQE of two).
inline constexpr std::uint32_t kUnpublishedSlack = 2;

struct Watched {
    std::uint32_t             qpn{0};
    std::uint8_t*             sq{nullptr};       // host VA of WQEBB 0; read, never written
    std::uint32_t*            dbr_snd{nullptr};  // host VA of the send counter; read, never written
    hijack::QpInfo            shape{};
    const void*               sq_umem{nullptr};  // unwatch keys: compared, never dereferenced
    const void*               dbr_umem{nullptr};
    std::uint32_t             last{0};
    bool                      seen_any{false};
    std::vector<std::uint8_t> buf;  // one ring: the copy the decoder reads
};

struct State {
    // Held for a whole polling round, and by unwatch. That is what makes unwatch
    // safe: once it returns the watcher is not inside this queue's memory and
    // never will be again, so the transport may free it.
    std::mutex                            mutex;
    std::vector<std::unique_ptr<Watched>> qps;
    std::thread                           thread;
    std::atomic<bool>                     stop{false};
    bool                                  started{false};  // under mutex

    std::atomic<std::uint32_t> qps_watched{0};
    std::atomic<std::uint32_t> qps_refused{0};
    std::atomic<std::uint64_t> wqebbs{0};
    std::atomic<std::uint64_t> held{0};
    std::atomic<std::uint64_t> lapped{0};
    std::atomic<std::uint64_t> polls{0};
};

// Leaked on purpose. A function-local static would be destroyed by the C++
// runtime at exit BEFORE the provider's __attribute__((destructor)) runs, and
// destroying a joinable std::thread is std::terminate — measured: every watched
// rank aborted with signal 6 just after the workload printed OK. The watcher's
// final reads of each queue happen at umem dereg, during the application's own
// teardown, so nothing is lost by never destroying this.
inline State& state() {
    static State* s = new State;
    return *s;
}

inline std::uint32_t read_counter(const Watched& w) {
    return be32toh(__atomic_load_n(w.dbr_snd, __ATOMIC_ACQUIRE)) & hijack::kProdMask;
}

// Does the slot for producer index `idx` hold that index's WQE? The control
// segment names both its queue pair and its own index, so a slot can be checked
// without trusting anything else. That matters because ibdevx never initialises
// the doorbell record: until its first post the counter is whatever
// posix_memalign's memory held, and decoding up to it would read a ring of
// stale heap as if it were sends.
inline bool slot_holds(const Watched& w, std::uint32_t idx) {
    const std::uint32_t mask = (1u << w.shape.log_wq_size) - 1u;
    const std::uint8_t* wqe = w.sq + static_cast<std::size_t>(idx & mask) * hijack::kWqebb;
    std::uint32_t       opmod_idx_opcode = 0, qpn_ds = 0;
    std::memcpy(&opmod_idx_opcode, wqe, 4);
    std::memcpy(&qpn_ds, wqe + 4, 4);
    return ((be32toh(opmod_idx_opcode) >> 8) & hijack::kProdMask) == (idx & hijack::kProdMask) &&
           (be32toh(qpn_ds) >> 8) == w.qpn;
}

// If this queue's producer index moved, copy the WQEBBs it passed over and
// decode them. Called with state().mutex held.
inline void drain_one(Watched& w, hijack::DeviceSendSink sink) {
    auto&               s = state();
    const std::uint32_t slots = 1u << w.shape.log_wq_size;
    const std::uint32_t end = read_counter(w);
    const std::uint32_t start = w.last;
    const std::uint32_t delta = hijack::prod_diff(end, start);
    if (delta == 0)
        return;

    if (!w.seen_any && (delta > slots || !slot_holds(w, start))) {
        // Before the first real post: the counter is the uninitialised doorbell
        // record talking. Wait for a slot that proves itself.
        s.held.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (delta > slots || !slot_holds(w, start)) {
        // More than a ring since the last look: those WQEs are gone. Resume at
        // the producer rather than decode what replaced them.
        s.lapped.fetch_add(delta, std::memory_order_relaxed);
        w.last = end;
        return;
    }

    const std::uint32_t mask = slots - 1u;
    const std::uint32_t first_slot = start & mask;
    const std::uint32_t tail = std::min(delta, slots - first_slot);
    std::memcpy(w.buf.data(), w.sq + static_cast<std::size_t>(first_slot) * hijack::kWqebb,
                static_cast<std::size_t>(tail) * hijack::kWqebb);
    if (tail < delta)
        std::memcpy(w.buf.data() + static_cast<std::size_t>(tail) * hijack::kWqebb, w.sq,
                    static_cast<std::size_t>(delta - tail) * hijack::kWqebb);

    // Could the producer have reached a slot while it was being copied? It
    // overwrites the slot for `start` only when it writes index start + slots,
    // and it may be a few WQEBBs ahead of what it has published.
    const std::uint32_t after = read_counter(w);
    if (hijack::prod_diff(after, start) + kUnpublishedSlack > slots) {
        s.lapped.fetch_add(delta, std::memory_order_relaxed);
        w.last = end;
        return;
    }

    hijack::parse_wqes(w.buf.data(), start, end, w.qpn, w.shape, sink);
    s.wqebbs.fetch_add(delta, std::memory_order_relaxed);
    w.last = end;
    w.seen_any = true;
}

inline void run() {
    auto& s = state();
    while (!s.stop.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lk(s.mutex);
            for (auto& w : s.qps)
                drain_one(*w, hijack::state().device_send_sink);
        }
        s.polls.fetch_add(1, std::memory_order_relaxed);
        if (poll_interval().count() > 0) {
            std::this_thread::sleep_for(poll_interval());
        } else {
            for (int i = 0; i < 32; ++i)
                cpu_relax();
        }
    }
}

// From the DEVX CREATE_QP decode: a queue pair created, so nothing posted yet.
inline void watch(std::uint32_t qpn, std::uint64_t sq_host, std::uint32_t log_wq_size,
                  std::uint8_t service_type, std::uint64_t dbr_host, const void* sq_umem,
                  const void* dbr_umem) {
    auto w = std::make_unique<Watched>();
    w->qpn = qpn;
    w->sq = reinterpret_cast<std::uint8_t*>(sq_host);
    w->dbr_snd = reinterpret_cast<std::uint32_t*>(dbr_host + kSndDbrOffset);
    w->shape = hijack::QpInfo{sq_host, log_wq_size, service_type};
    w->sq_umem = sq_umem;
    w->dbr_umem = dbr_umem;
    w->buf.resize(static_cast<std::size_t>(1u << log_wq_size) * hijack::kWqebb);
    if (debug_on())
        std::fprintf(stderr, "[sqwatch] watch qpn=%u sq=%#llx slots=%u dbr=%#llx\n", qpn,
                     static_cast<unsigned long long>(sq_host), 1u << log_wq_size,
                     static_cast<unsigned long long>(dbr_host));

    auto&                       s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.qps.push_back(std::move(w));
    s.qps_watched.fetch_add(1, std::memory_order_relaxed);
    if (!s.started) {
        s.started = true;
        s.thread = std::thread(run);
    }
}

inline void refused(std::uint32_t qpn, const char* why) {
    state().qps_refused.fetch_add(1, std::memory_order_relaxed);
    if (debug_on())
        std::fprintf(stderr, "[sqwatch] NOT watching qpn=%u: %s\n", qpn, why);
}

// From the umem dereg hook, which runs before the transport frees the buffer:
// one last drain, then forget the queue. Keyed on the umem pointer, compared
// only — it may already be freed by the time this runs.
inline void unwatch_umem(const void* umem) {
    if (!umem)
        return;
    auto&                       s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    for (auto it = s.qps.begin(); it != s.qps.end();) {
        if ((*it)->sq_umem == umem || (*it)->dbr_umem == umem) {
            drain_one(**it, hijack::state().device_send_sink);
            it = s.qps.erase(it);
        } else {
            ++it;
        }
    }
}

inline void shutdown(int rank) {
    auto& s = state();
    if (s.stop.exchange(true, std::memory_order_acq_rel))
        return;  // already stopped and reported
    if (s.thread.joinable())
        s.thread.join();
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        for (auto& w : s.qps)
            drain_one(*w, hijack::state().device_send_sink);
    }
    auto& hs = hijack::state();
    std::fprintf(stderr,
                 "[pcomm] sq-watch rank=%d qps_watched=%u refused=%u wqebbs=%llu sends=%llu "
                 "(unknown_op=%llu truncated=%llu) held=%llu lapped=%llu polls=%llu\n",
                 rank, s.qps_watched.load(), s.qps_refused.load(),
                 static_cast<unsigned long long>(s.wqebbs.load()),
                 static_cast<unsigned long long>(hs.sends_emitted.load()),
                 static_cast<unsigned long long>(hs.sends_unknown_op.load()),
                 static_cast<unsigned long long>(hs.sends_truncated.load()),
                 static_cast<unsigned long long>(s.held.load()),
                 static_cast<unsigned long long>(s.lapped.load()),
                 static_cast<unsigned long long>(s.polls.load()));
}

}  // namespace mlx5::sqwatch
