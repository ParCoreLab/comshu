#pragma once
// The passive send-queue observer: the data plane of a DEVX queue pair whose
// send queue and doorbell record are both host memory — NVSHMEM's ibdevx
// transport above all. tests/hpc/cases/11_ibdevx_peermem/parity/README.md
// measured the gap this closes: under ibdevx neither comshu nor SigmaTrace
// recorded one byte of the 16,384 the workload moved through the NIC.
//
// Opt-in: `comshu run --watch-send-queues`, which sets COMSHU_SQ_WATCH=1. What it
// saw and missed is one Mlx5SendCoverage row per rank; the design, the rejected
// alternatives and the measurements are in
// plans/PLAN-20260914-1750-the-ibdevx-data-plane-is-a-host-send-queue.
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
// TWO THREADS, AND THE WATCHER NEVER RECORDS. The first version copied, decoded
// and recorded on one thread, and lost 7,000-12,000 WQEBBs per rank on
// `1024 20` in exactly one event per run — even with the watcher on a CPU of its
// own (du04, 2026-09-14: 2-4 involuntary context switches, a poll every ~1 us).
// The longest single batch was 2.1-2.4 ms, always at batch ~800: the moment the
// watcher's first 4,096-row arena filled and comshu_engine_arena_flush ran inline
// — a synchronous drain plus the process's one-time GpuDevice emission, which
// queries the CUDA driver. The proxy posted straight through it and lapped the
// 1,024-slot ring many times over. Recording is a stall waiting to happen, so it
// moved: the watcher only copies WQEBBs into a staging ring, and a decoder
// thread parses and records them. A decoder stall now costs staging space, not
// data, and 16 MiB of staging is ~262k WQEBBs.
//
// What it costs, stated rather than discovered later:
//   * a row's time is a Window, not an Instant: the WQE was published after the
//     watcher's previous look at its queue and before this one (both bracketed
//     by TSC reads around the doorbell-record read, carried through staging).
//     It is never the post itself, and the record says so (SendSubmitter::SqWatch);
//   * the watcher SPINS, which costs a core. Sleeping 20 us between polls —
//     about 60 us once the kernel rounds it — let the CPU proxy lap the ring
//     (one run decoded 9 of ~4,100 WQEs). COMSHU_SQ_WATCH_US=<n> sleeps anyway;
//   * and that core is NOT the rank's own. Open MPI binds a rank to one core by
//     default, and the watcher inherits it: sharing two hardware threads with
//     the main thread and the very proxy it watches, it took 7,700-16,000
//     involuntary context switches and kept ~20% of `1024 20`. Unbound, the same
//     run was lossless. So by default the watcher moves to the CPUs sharing the
//     rank's L3 and the decoder to its NUMA node (place_thread, which has the
//     measurements); under SLURM the kernel intersects that with the job's
//     cpuset, so it never leaves the allocation;
//   * a window of one ring between two polls, and of the staging ring between
//     watcher and decoder. Anything lost to either is counted — `lapped` and
//     `dropped` — and never decoded from whatever bytes replaced it. staged +
//     lapped + dropped is everything the producer published.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <endian.h>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#include "comshu/helpers/clock.hpp"
#include "uar_hijack.hpp"

namespace mlx5::sqwatch {

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_SQ_WATCH");
        return v && *v == '1';
    }();
    return on;
}

inline bool debug_on() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_SQ_WATCH_DEBUG");
        return v && *v == '1';
    }();
    return on;
}

// Zero, the default, means spin.
inline std::chrono::microseconds poll_interval() {
    static const auto us = [] {
        const char* v = std::getenv("COMSHU_SQ_WATCH_US");
        const long  n = v ? std::strtol(v, nullptr, 10) : 0;
        return std::chrono::microseconds(n > 0 ? n : 0);
    }();
    return us;
}

inline std::size_t stage_bytes() {
    static const std::size_t b = [] {
        const char* v = std::getenv("COMSHU_SQ_WATCH_STAGE_MB");
        const long  mb = v ? std::strtol(v, nullptr, 10) : 0;
        return static_cast<std::size_t>(mb > 0 ? mb : 16) << 20;
    }();
    return b;
}

// MEASUREMENT ONLY, like COMSHU_HIJACK_NO_DECODE: how much of the watcher's cost
// to the application is which part of it. COMSHU_SQ_WATCH_STAGE=
//   poll   read the doorbell records and nothing else — no send-queue read;
//   copy   also copy WQEBBs into staging, but the decoder records nothing;
//   full   the capture (default).
// `poll` and `copy` produce captures with no send rows, which is exactly the
// shape of a broken watcher, so neither is a capture mode and neither gets a
// launcher flag.
enum class StageMode { Full, Copy, Poll };
inline StageMode stage_mode() {
    static const StageMode m = [] {
        const char* v = std::getenv("COMSHU_SQ_WATCH_STAGE");
        if (v && std::strcmp(v, "poll") == 0)
            return StageMode::Poll;
        if (v && std::strcmp(v, "copy") == 0)
            return StageMode::Copy;
        return StageMode::Full;
    }();
    return m;
}

// This thread's CPU time, user + system, in microseconds; -1 if unknown.
inline long thread_cpu_us() {
    rusage ru{};
    if (getrusage(RUSAGE_THREAD, &ru) != 0)
        return -1;
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000L + ru.ru_utime.tv_usec +
           ru.ru_stime.tv_usec;
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
    std::uint32_t  qpn{0};
    std::uint8_t*  sq{nullptr};       // host VA of WQEBB 0; read, never written
    std::uint32_t* dbr_snd{nullptr};  // host VA of the send counter; read, never written
    hijack::QpInfo shape{};
    const void*    sq_umem{nullptr};  // unwatch keys: compared, never dereferenced
    const void*    dbr_umem{nullptr};
    std::uint32_t  last{0};
    bool           seen_any{false};
    // TSC taken just before the counter read of the last look that moved `last`
    // or found nothing new: everything past `last` was published after it. The
    // lower end of the next row's Window.
    std::uint64_t look_ticks{0};
};

// --- staging ring: the watcher writes, the decoder reads ----------------------
//
// Single producer (the watcher, or an unwatch/shutdown drain — all under the
// State mutex, so never two at once) and single consumer (the decoder thread).
// Each entry is one WQEBB-sized header followed by the WQEBBs it describes, and
// never wraps: an entry that would is preceded by a padding header (n == 0) that
// fills the ring to its end, so the decoder always hands parse_wqes one
// contiguous run.
struct alignas(64) StageHdr {
    std::uint32_t qpn;
    std::uint32_t start;  // producer index of the first WQEBB
    std::uint32_t n;      // WQEBBs that follow; 0 = padding to the end of the ring
    std::uint32_t log_wq_size;
    std::uint64_t ticks_lo;  // published after this (TSC)...
    std::uint64_t ticks_hi;  // ...and before this: the rows' Window
    std::uint8_t  service_type;
};
static_assert(sizeof(StageHdr) == hijack::kWqebb, "a header is one WQEBB");

struct Stage {
    std::unique_ptr<std::uint8_t[]> buf;  // not zero-filled: pages fault in as used
    std::size_t                     cap{0};
    std::atomic<std::uint64_t>      head{0};  // bytes published by the watcher
    std::atomic<std::uint64_t>      tail{0};  // bytes consumed by the decoder
};

// The watcher's coverage at shutdown — what Mlx5SendCoverage records. Kept in
// this header's own terms, because the records live with the provider, which
// hands in a sink to write them (the same split as hijack::set_sinks).
struct Coverage {
    std::uint64_t published{0};  // staged + lapped + dropped
    std::uint64_t decoded{0};
    std::uint64_t lapped{0};
    std::uint64_t dropped{0};
    std::uint64_t rows{0};
    std::uint64_t rows_unknown_op{0};
    std::uint64_t rows_truncated{0};
    std::uint32_t queues{0};
    std::uint32_t refused{0};
    std::uint32_t held{0};
};
using CoverageSink = void (*)(const Coverage&);

struct State {
    // Held for a whole polling round, and by unwatch. That is what makes unwatch
    // safe: once it returns the watcher is not inside this queue's memory and
    // never will be again, so the transport may free it.
    std::mutex                            mutex;
    std::vector<std::unique_ptr<Watched>> qps;
    std::thread                           watcher;
    std::thread                           decoder;
    std::atomic<bool>                     stop{false};
    std::atomic<bool>                     decoder_stop{false};
    bool                                  started{false};  // under mutex
    Stage                                 stage;

    std::atomic<std::uint32_t> qps_watched{0};
    std::atomic<std::uint32_t> qps_refused{0};
    // Why, by reason, with the first refused queue's CREATE_QP fields. A bare
    // "refused=55" is what MN5's case 11 first reported, and nobody can act on it.
    struct Refusal {
        const char*   why;
        std::uint32_t n;
        std::string   first;
    };
    std::vector<Refusal>       refusals;    // under mutex
    std::atomic<std::uint64_t> staged{0};   // WQEBBs the watcher copied and published
    std::atomic<std::uint64_t> decoded{0};  // WQEBBs the decoder parsed and recorded
    std::atomic<std::uint64_t> held{0};
    std::atomic<std::uint64_t> polls{0};

    // Where any loss comes from. `lapped_early`: the producer was already more
    // than a ring ahead when the watcher looked. `lapped_late`: the copy was
    // taken and discarded because the producer came within reach of it while
    // copying. `dropped`: the staging ring was full — the decoder is behind.
    std::atomic<std::uint64_t> lapped_early{0};
    std::atomic<std::uint64_t> lapped_late{0};
    std::atomic<std::uint64_t> dropped{0};

    // Timing, per thread, because one stall is enough to lose a ring and a total
    // hides it.
    std::atomic<std::uint64_t> batches{0};
    std::atomic<std::uint32_t> max_delta{0};
    std::atomic<std::uint64_t> copy_ns{0};
    std::uint64_t              max_batch_ns{0};  // watcher thread only
    std::uint64_t              max_batch_at{0};
    // The longest time between the starts of two polling rounds, and the round
    // it ended. A spinning watcher that is preempted for longer than a ring
    // takes to fill laps it however much CPU it gets overall: MN5 job 45891600
    // lost 1,450 WQEBBs on rank 0 in one gap with 4,786 of 4,798 ms on CPU.
    std::uint64_t              max_gap_ns{0};  // watcher thread only
    std::uint64_t              max_gap_at{0};
    std::uint64_t              first_batch_ns{0};
    std::atomic<std::uint64_t> decode_ns{0};
    std::uint64_t              max_decode_ns{0};  // decoder thread only
    // The watcher thread's own view of its scheduling: which CPU it started on,
    // how many it may run on, and how often the kernel took the CPU away.
    int  cpu_start{-1};
    int  cpu_end{-1};  // where it finished: an `auto` watcher can migrate across L3s
    int  affinity_cpus{-1};
    long csw_voluntary{-1};
    long csw_involuntary{-1};
    // What the two threads actually consumed, against how long the watcher ran:
    // the cost question, which wall time cannot answer on a run whose wall time
    // is startup.
    long watcher_cpu_us{-1};
    long watcher_wall_us{-1};
    long decoder_cpu_us{-1};

    // This door's own rows. hijack::state()'s counters are process-wide and
    // shared by every door that decodes through parse_wqes.
    std::atomic<std::uint64_t> rows{0};
    std::atomic<std::uint64_t> rows_unknown_op{0};
    std::atomic<std::uint64_t> rows_truncated{0};

    CoverageSink coverage_sink{nullptr};  // set by the provider before any watch()
    int          rank{-1};
};

// Leaked on purpose. A function-local static would be destroyed by the C++
// runtime at exit BEFORE the provider's __attribute__((destructor)) runs, and
// destroying a joinable std::thread is std::terminate — measured: every watched
// rank aborted with signal 6 just after the workload printed OK. The final reads
// of each queue happen at umem dereg, during the application's own teardown, so
// nothing is lost by never destroying this.
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

// Room for a header plus n WQEBBs, contiguous; nullptr if the ring is full.
// Writes the padding header itself when the entry would cross the end, and
// returns the position the entry starts at through `pos`.
inline std::uint8_t* stage_reserve(Stage& st, std::uint32_t n, std::uint64_t& pos) {
    const std::size_t   need = static_cast<std::size_t>(n + 1) * hijack::kWqebb;
    std::uint64_t       head = st.head.load(std::memory_order_relaxed);
    const std::uint64_t tail = st.tail.load(std::memory_order_acquire);
    std::size_t         off = head % st.cap;
    const std::size_t   pad = off + need > st.cap ? st.cap - off : 0;
    if (head + pad + need - tail > st.cap)
        return nullptr;
    if (pad) {
        // Not published by itself: if the entry after it is discarded, the next
        // reserve simply writes the same padding again.
        reinterpret_cast<StageHdr*>(st.buf.get() + off)->n = 0;
        head += pad;
        off = 0;
    }
    pos = head;
    return st.buf.get() + off;
}

// If this queue's producer index moved, copy the WQEBBs it passed over into
// staging. Called with state().mutex held; records nothing.
inline void drain_one(Watched& w) {
    auto&               s = state();
    const std::uint32_t slots = 1u << w.shape.log_wq_size;
    // The TSC either side of the counter read. Whatever this look finds new was
    // published after w.look_ticks and before `seen` — an exact bound, which is
    // what the row's Window says, and all this passive door can honestly say.
    const std::uint64_t look = comshu::helpers::TscClock::now();
    const std::uint32_t end = read_counter(w);
    const std::uint64_t seen = comshu::helpers::TscClock::now();
    const std::uint64_t since = w.look_ticks;
    const std::uint32_t start = w.last;
    const std::uint32_t delta = hijack::prod_diff(end, start);
    if (delta == 0) {
        w.look_ticks = look;  // nothing new: whatever comes next is newer than this look
        return;
    }
    for (std::uint32_t m = s.max_delta.load(std::memory_order_relaxed);
         delta > m && !s.max_delta.compare_exchange_weak(m, delta, std::memory_order_relaxed);) {
    }
    if (stage_mode() == StageMode::Poll) {  // measurement: the counter and nothing else
        s.staged.fetch_add(delta, std::memory_order_relaxed);
        w.last = end;
        w.look_ticks = look;
        w.seen_any = true;
        return;
    }

    if (!w.seen_any && (delta > slots || !slot_holds(w, start))) {
        // Before the first real post: the counter is the uninitialised doorbell
        // record talking. Wait for a slot that proves itself.
        s.held.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (delta > slots || !slot_holds(w, start)) {
        // More than a ring since the last look: those WQEs are gone. Resume at
        // the producer rather than decode what replaced them.
        s.lapped_early.fetch_add(delta, std::memory_order_relaxed);
        w.last = end;
        w.look_ticks = look;
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto    t0 = clock::now();
    std::uint64_t pos = 0;
    std::uint8_t* entry = stage_reserve(s.stage, delta, pos);
    if (!entry) {
        s.dropped.fetch_add(delta, std::memory_order_relaxed);
        w.last = end;
        w.look_ticks = look;
        w.seen_any = true;
        return;
    }
    auto* hdr = reinterpret_cast<StageHdr*>(entry);
    hdr->qpn = w.qpn;
    hdr->start = start;
    hdr->n = delta;
    hdr->log_wq_size = w.shape.log_wq_size;
    hdr->service_type = w.shape.service_type;
    hdr->ticks_lo = since;
    hdr->ticks_hi = seen;

    std::uint8_t*       dst = entry + hijack::kWqebb;
    const std::uint32_t mask = slots - 1u;
    const std::uint32_t first_slot = start & mask;
    const std::uint32_t tail = std::min(delta, slots - first_slot);
    std::memcpy(dst, w.sq + static_cast<std::size_t>(first_slot) * hijack::kWqebb,
                static_cast<std::size_t>(tail) * hijack::kWqebb);
    if (tail < delta)
        std::memcpy(dst + static_cast<std::size_t>(tail) * hijack::kWqebb, w.sq,
                    static_cast<std::size_t>(delta - tail) * hijack::kWqebb);

    // Could the producer have reached a slot while it was being copied? It
    // overwrites the slot for `start` only when it writes index start + slots,
    // and it may be a few WQEBBs ahead of what it has published.
    const std::uint32_t after = read_counter(w);
    if (hijack::prod_diff(after, start) + kUnpublishedSlack > slots) {
        s.lapped_late.fetch_add(delta, std::memory_order_relaxed);
        w.last = end;
        w.look_ticks = look;
        return;
    }

    s.stage.head.store(pos + static_cast<std::uint64_t>(delta + 1) * hijack::kWqebb,
                       std::memory_order_release);
    s.staged.fetch_add(delta, std::memory_order_relaxed);
    w.last = end;
    w.look_ticks = look;
    w.seen_any = true;

    const std::uint64_t batch_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
    s.copy_ns.fetch_add(batch_ns, std::memory_order_relaxed);
    const std::uint64_t batch_no = s.batches.fetch_add(1, std::memory_order_relaxed);
    if (batch_no == 0)
        s.first_batch_ns = batch_ns;
    if (batch_ns > s.max_batch_ns) {
        s.max_batch_ns = batch_ns;
        s.max_batch_at = batch_no;
    }
}

// Everything published so far: parse, record, give the space back. Decoder
// thread only.
inline void decode_staged() {
    auto&         s = state();
    auto&         st = s.stage;
    std::uint64_t tail = st.tail.load(std::memory_order_relaxed);
    while (tail < st.head.load(std::memory_order_acquire)) {
        const std::size_t off = tail % st.cap;
        const auto*       hdr = reinterpret_cast<const StageHdr*>(st.buf.get() + off);
        if (hdr->n == 0) {
            tail += st.cap - off;  // padding to the end of the ring
            st.tail.store(tail, std::memory_order_release);
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (stage_mode() != StageMode::Copy) {  // measurement: Copy stages and records nothing
            const hijack::ParseCounts pc =
                hijack::parse_wqes(st.buf.get() + off + hijack::kWqebb, hdr->start,
                                   (hdr->start + hdr->n) & hijack::kProdMask, hdr->qpn,
                                   hijack::QpInfo{0, hdr->log_wq_size, hdr->service_type},
                                   hijack::state().device_send_sink, hijack::kSubmitterSqWatch,
                                   hdr->ticks_lo, hdr->ticks_hi);
            s.rows.fetch_add(pc.rows, std::memory_order_relaxed);
            s.rows_unknown_op.fetch_add(pc.unknown_op, std::memory_order_relaxed);
            s.rows_truncated.fetch_add(pc.truncated, std::memory_order_relaxed);
        }
        const std::uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count();
        s.decode_ns.fetch_add(ns, std::memory_order_relaxed);
        s.max_decode_ns = std::max(s.max_decode_ns, ns);
        s.decoded.fetch_add(hdr->n, std::memory_order_relaxed);
        tail += static_cast<std::uint64_t>(hdr->n + 1) * hijack::kWqebb;
        st.tail.store(tail, std::memory_order_release);  // space back as soon as it is free
    }
}

inline void place_thread(bool is_watcher);  // with the watcher's own setup, below

inline void decode_run() {
    auto& s = state();
    place_thread(false);
    for (;;) {
        const bool stopping = s.decoder_stop.load(std::memory_order_acquire);
        decode_staged();
        if (stopping) {
            s.decoder_cpu_us = thread_cpu_us();
            return;  // one full pass after stop was seen, so nothing published is left
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// Block until the decoder has consumed everything published up to `upto`, for
// at most `limit`. Used where a caller must know the rows are recorded before it
// lets the application continue tearing down.
inline bool wait_decoded(std::uint64_t upto, std::chrono::milliseconds limit) {
    auto&      st = state().stage;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (st.tail.load(std::memory_order_acquire) < upto) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    return true;
}

// A sysfs cpulist file ("0-3,128-131") into `out`. False if it cannot be read
// or is not a cpulist — nothing is guessed from a malformed one.
inline bool read_cpulist(const char* path, cpu_set_t& out) {
    std::FILE* f = std::fopen(path, "r");
    if (!f)
        return false;
    char       list[1024] = {};
    const bool got = std::fgets(list, sizeof list, f) != nullptr;
    std::fclose(f);
    if (!got)
        return false;
    CPU_ZERO(&out);
    for (char* p = list; *p && *p != '\n';) {
        char*      next = nullptr;
        const long lo = std::strtol(p, &next, 10);
        if (next == p)
            break;
        long hi = lo;
        if (*next == '-')
            hi = std::strtol(next + 1, &next, 10);
        for (long c = lo; c <= hi && c < CPU_SETSIZE; ++c)
            CPU_SET(static_cast<int>(c), &out);
        if (*next != ',')
            break;
        p = next + 1;
    }
    return CPU_COUNT(&out) > 0;
}

// The CPUs sharing a NUMA node with `cpu`. False if sysfs does not say, in
// which case the caller widens to everything instead.
inline bool node_cpus_of(int cpu, cpu_set_t& out) {
    char path[96];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d", cpu);
    DIR* dir = opendir(path);
    if (!dir)
        return false;
    int node = -1;
    while (dirent* e = readdir(dir))
        if (std::strncmp(e->d_name, "node", 4) == 0 && std::isdigit(e->d_name[4]))
            node = std::atoi(e->d_name + 4);
    closedir(dir);
    if (node < 0)
        return false;
    std::snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
    return read_cpulist(path, out);
}

// The CPUs sharing `cpu`'s last-level (L3) cache: the cache index whose level
// is 3, not index3 by assumption — the numbering is the kernel's, not a
// standard's.
inline bool l3_cpus_of(int cpu, cpu_set_t& out) {
    for (int i = 0; i < 8; ++i) {
        char path[128];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu,
                      i);
        std::FILE* f = std::fopen(path, "r");
        if (!f)
            return false;  // indices are contiguous: past the last one
        int        level = 0;
        const bool got = std::fscanf(f, "%d", &level) == 1;
        std::fclose(f);
        if (!got || level != 3)
            continue;
        std::snprintf(path, sizeof path,
                      "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", cpu, i);
        return read_cpulist(path, out);
    }
    return false;
}

// Where a watcher thread runs. COMSHU_SQ_WATCH_CPU:
//   auto (default)  the watcher on the CPUs sharing the rank's L3 cache, minus
//                   the rank's own binding; the NUMA node if there are none.
//                   The decoder always takes the NUMA node — it reads only the
//                   staging ring, never a line the proxy writes.
//   node            both threads anywhere on the rank's NUMA node (what auto
//                   was until the cost measurement below);
//   bound           the rank's binding, unchanged — the configuration that
//                   loses data, kept so it can be measured;
//   <base>          the watcher alone on CPU base + local rank; the decoder on
//                   the node.
//
// WHY THE L3. The watcher's cost to the application is distance, not CPU time:
// every WQEBB and doorbell-record line it reads is one the proxy must take back
// before writing it again. Measured on du04 (EPYC 7742, a 4-core L3 per CCX),
// `1024 20000`, medians of 5 interleaved runs, traffic phase against bare:
// same L3 1.9x, NUMA node (the old auto, a random L3 that migrated mid-run)
// 2.2x, another L3 on the socket 3.1x, the other socket 4.8x — and this `auto`,
// same method, 1.15x, every run inside the rank's L3. On a machine whose
// L3 spans the socket this is simply the socket. Its known limit: ranks that
// fill an L3 domain leave the watcher beside them, which is `bound`'s price.
//
// The kernel intersects any request with the cgroup cpuset, so under SLURM none
// of these leaves the job's allocation; if nothing is left, the call fails and
// the thread keeps the binding it had, which is reported rather than hidden.
inline void place_thread(bool is_watcher) {
    const char* v = std::getenv("COMSHU_SQ_WATCH_CPU");
    const char* mode = v && *v ? v : "auto";
    if (std::strcmp(mode, "bound") == 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    // A pinned watcher's decoder still goes to the node. It used to stay on the
    // rank's binding, where it competed with the proxy and fell behind — and
    // the first pinned measurements read the resulting drops as a recording-
    // bandwidth limit. `auto`, with the decoder on the node, recorded the same
    // volume faster and dropped nothing.
    if (is_watcher && std::isdigit(static_cast<unsigned char>(mode[0]))) {
        const char* local = std::getenv("OMPI_COMM_WORLD_LOCAL_RANK");
        CPU_SET(std::atoi(mode) + (local ? std::atoi(local) : 0), &set);
    } else {
        const int here = sched_getcpu();
        bool      placed = false;
        if (is_watcher && std::strcmp(mode, "auto") == 0 && here >= 0 && l3_cpus_of(here, set)) {
            cpu_set_t own;
            CPU_ZERO(&own);
            if (pthread_getaffinity_np(pthread_self(), sizeof own, &own) == 0)
                for (int c = 0; c < CPU_SETSIZE; ++c)
                    if (CPU_ISSET(c, &own))
                        CPU_CLR(c, &set);
            placed = CPU_COUNT(&set) > 0;
        }
        if (!placed) {
            CPU_ZERO(&set);
            if (here < 0 || !node_cpus_of(here, set))
                for (int c = 0; c < CPU_SETSIZE; ++c)
                    CPU_SET(c, &set);
        }
    }
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
        std::fprintf(stderr,
                     "[sqwatch] could not place the %s thread (COMSHU_SQ_WATCH_CPU=%s); it "
                     "keeps the rank's binding\n",
                     is_watcher ? "watcher" : "decoder", mode);
}

inline void run() {
    auto& s = state();
    place_thread(true);
    const auto run_start = std::chrono::steady_clock::now();
    // A sleeping watcher asks for exact wakeups. The default 50 us timer slack is
    // why a requested 20 us sleep measured ~60 us, and a ring can lap inside the
    // difference. Per-thread, so the application's own sleeps are untouched.
    if (poll_interval().count() > 0)
        prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
    s.cpu_start = sched_getcpu();
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof set, &set) == 0)
        s.affinity_cpus = CPU_COUNT(&set);
    auto round_start = std::chrono::steady_clock::now();
    while (!s.stop.load(std::memory_order_acquire)) {
        {
            const auto now = std::chrono::steady_clock::now();
            const auto gap = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - round_start).count());
            if (gap > s.max_gap_ns) {
                s.max_gap_ns = gap;
                s.max_gap_at = s.polls.load(std::memory_order_relaxed);
            }
            round_start = now;
        }
        {
            std::lock_guard<std::mutex> lk(s.mutex);
            for (auto& w : s.qps)
                drain_one(*w);
        }
        s.polls.fetch_add(1, std::memory_order_relaxed);
        if (poll_interval().count() > 0) {
            std::this_thread::sleep_for(poll_interval());
        } else {
            for (int i = 0; i < 32; ++i)
                cpu_relax();
        }
    }
    rusage ru{};
    if (getrusage(RUSAGE_THREAD, &ru) == 0) {
        s.csw_voluntary = ru.ru_nvcsw;
        s.csw_involuntary = ru.ru_nivcsw;
    }
    s.cpu_end = sched_getcpu();
    s.watcher_cpu_us = thread_cpu_us();
    s.watcher_wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - run_start)
                            .count();
}

inline void shutdown(int rank);  // below; watch() registers it with atexit

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
    w->look_ticks = comshu::helpers::TscClock::now();  // just created: nothing was posted before now
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
        s.stage.cap = stage_bytes();
        s.stage.buf.reset(new std::uint8_t[s.stage.cap]);
        s.decoder = std::thread(decode_run);
        s.watcher = std::thread(run);
        // The coverage row must be written after the application's last send
        // and before the tracer finalizes. The tracer registered finalize with
        // atexit when it started, before any queue pair existed; atexit is LIFO,
        // so a handler registered now runs first — while the capture is still
        // open. The provider's destructor also calls shutdown(), but it runs
        // after finalize and can only print; shutdown() does its work once.
        std::atexit([] { shutdown(state().rank); });
    }
}

// From the provider's constructor, before any queue pair can be watched.
inline void set_coverage_sink(CoverageSink sink, int rank) {
    auto& s = state();
    s.coverage_sink = sink;
    s.rank = rank;
}

// `fields` is the queue's CREATE_QP as the watcher judged it; the first one of
// each reason is kept for the shutdown summary.
inline void refused(std::uint32_t qpn, const char* why, const std::string& fields) {
    auto& s = state();
    s.qps_refused.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        auto it = std::find_if(s.refusals.begin(), s.refusals.end(), [&](const State::Refusal& r) {
            return std::strcmp(r.why, why) == 0;
        });
        if (it == s.refusals.end())
            s.refusals.push_back({why, 1, fields});
        else
            ++it->n;
    }
    if (debug_on())
        std::fprintf(stderr, "[sqwatch] NOT watching qpn=%u: %s (%s)\n", qpn, why, fields.c_str());
}

// From the umem dereg hook, which runs before the transport frees the buffer:
// one last drain into staging, then forget the queue — and wait, briefly, for
// the decoder to record it, so the rows exist before teardown goes further.
// Keyed on the umem pointer, compared only: it may already be freed.
inline void unwatch_umem(const void* umem) {
    if (!umem)
        return;
    auto&         s = state();
    std::uint64_t upto = 0;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.started)
            return;
        for (auto it = s.qps.begin(); it != s.qps.end();) {
            if ((*it)->sq_umem == umem || (*it)->dbr_umem == umem) {
                drain_one(**it);
                it = s.qps.erase(it);
            } else {
                ++it;
            }
        }
        upto = s.stage.head.load(std::memory_order_relaxed);
    }
    wait_decoded(upto, std::chrono::milliseconds(200));
}

inline void shutdown(int rank) {
    auto& s = state();
    if (s.stop.exchange(true, std::memory_order_acq_rel))
        return;  // already stopped and reported
    if (s.watcher.joinable())
        s.watcher.join();
    std::uint64_t upto = 0;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        for (auto& w : s.qps)
            drain_one(*w);
        upto = s.stage.head.load(std::memory_order_relaxed);
    }
    wait_decoded(upto, std::chrono::milliseconds(500));
    s.decoder_stop.store(true, std::memory_order_release);
    if (s.decoder.joinable())
        s.decoder.join();

    const std::uint64_t lapped = s.lapped_early.load() + s.lapped_late.load();
    if (s.coverage_sink) {
        Coverage c;
        c.published = s.staged.load() + lapped + s.dropped.load();
        c.decoded = s.decoded.load();
        c.lapped = lapped;
        c.dropped = s.dropped.load();
        c.rows = s.rows.load();
        c.rows_unknown_op = s.rows_unknown_op.load();
        c.rows_truncated = s.rows_truncated.load();
        c.queues = s.qps_watched.load();
        c.refused = s.qps_refused.load();
        c.held = static_cast<std::uint32_t>(s.held.load());
        s.coverage_sink(c);
    }

    const std::uint64_t undecoded =
        (s.stage.head.load() - s.stage.tail.load()) / hijack::kWqebb;  // headers included
    std::fprintf(stderr,
                 "[comshu] sq-watch rank=%d qps_watched=%u refused=%u staged=%llu decoded=%llu "
                 "sends=%llu (unknown_op=%llu truncated=%llu) held=%llu lapped=%llu "
                 "dropped=%llu polls=%llu\n",
                 rank, s.qps_watched.load(), s.qps_refused.load(),
                 static_cast<unsigned long long>(s.staged.load()),
                 static_cast<unsigned long long>(s.decoded.load()),
                 static_cast<unsigned long long>(s.rows.load()),
                 static_cast<unsigned long long>(s.rows_unknown_op.load()),
                 static_cast<unsigned long long>(s.rows_truncated.load()),
                 static_cast<unsigned long long>(s.held.load()),
                 static_cast<unsigned long long>(s.lapped_early.load() + s.lapped_late.load()),
                 static_cast<unsigned long long>(s.dropped.load()),
                 static_cast<unsigned long long>(s.polls.load()));
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        for (const auto& r : s.refusals)
            std::fprintf(stderr, "[comshu] sq-watch rank=%d refused %u: %s — first: %s\n", rank, r.n,
                         r.why, r.first.c_str());
    }
    std::fprintf(stderr,
                 "[comshu] sq-watch rank=%d lapped_early=%llu lapped_late=%llu batches=%llu "
                 "max_delta=%u copy_us=%llu first_batch_us=%llu max_batch_us=%llu@%llu "
                 "decode_us=%llu max_decode_us=%llu undecoded=%llu cpu=%d affinity=%d "
                 "csw_vol=%ld csw_invol=%ld\n",
                 rank, static_cast<unsigned long long>(s.lapped_early.load()),
                 static_cast<unsigned long long>(s.lapped_late.load()),
                 static_cast<unsigned long long>(s.batches.load()), s.max_delta.load(),
                 static_cast<unsigned long long>(s.copy_ns.load() / 1000),
                 static_cast<unsigned long long>(s.first_batch_ns / 1000),
                 static_cast<unsigned long long>(s.max_batch_ns / 1000),
                 static_cast<unsigned long long>(s.max_batch_at),
                 static_cast<unsigned long long>(s.decode_ns.load() / 1000),
                 static_cast<unsigned long long>(s.max_decode_ns / 1000),
                 static_cast<unsigned long long>(undecoded), s.cpu_start, s.affinity_cpus,
                 s.csw_voluntary, s.csw_involuntary);
    std::fprintf(stderr,
                 "[comshu] sq-watch rank=%d watcher_cpu_ms=%ld watcher_wall_ms=%ld "
                 "decoder_cpu_ms=%ld poll_us=%lld cpu=%d->%d max_gap_us=%llu@%llu\n",
                 rank, s.watcher_cpu_us / 1000, s.watcher_wall_us / 1000, s.decoder_cpu_us / 1000,
                 static_cast<long long>(poll_interval().count()), s.cpu_start, s.cpu_end,
                 static_cast<unsigned long long>(s.max_gap_ns / 1000),
                 static_cast<unsigned long long>(s.max_gap_at));
}

}  // namespace mlx5::sqwatch
