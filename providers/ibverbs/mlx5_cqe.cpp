// Implementation of the CQE probe declared in mlx5_cqe.hpp — see that file for
// the question this answers and for why the mlx5dv include lives here alone.

#include "mlx5_cqe.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <infiniband/verbs.h>

#if PCOMM_HAVE_MLX5DV
#include <dlfcn.h>
#include <endian.h>
#include <infiniband/mlx5dv.h>
#endif

namespace ibv::cqeprobe {

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("PCOMM_CQE_PROBE");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

#if !PCOMM_HAVE_MLX5DV

// No mlx5dv headers at build time: the probe cannot exist, and says so once
// rather than being silently absent — an absent diagnostic that looks like a
// negative result is the failure mode this whole file is written against.
//
// The production entry points return `valid = false`, which their callers must
// treat as "record nothing". A capture built this way simply has no
// ib_recv_cqe or ib_srq_recv_slot rows, and every other table is unchanged —
// the honest statement about a host whose NIC we cannot interrogate.
RecvCqe read_recv_cqe(ibv_cq*, int, int, const ibv_wc&) {
    return {};
}
SrqSlot srq_slot(ibv_srq*, unsigned) {
    return {};
}

void on_poll(ibv_cq*, int, ibv_wc*) {
    if (!enabled())
        return;
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[pcomm-cqeprobe] built WITHOUT mlx5dv headers — probe unavailable\n");
        std::fflush(stderr);
    }
}

#else

// mlx5's own CQE opcode classes (op_own >> 4). Only the split between requester
// and responder matters here, so the rest are named for the log and nothing
// branches on them.
enum : std::uint8_t {
    kCqeReq = 0,
    kCqeRespWrImm = 1,
    kCqeRespSend = 2,
    kCqeRespSendImm = 3,
    kCqeRespSendInv = 4,
};
inline bool is_responder(std::uint8_t op) {
    return op >= kCqeRespWrImm && op <= kCqeRespSendInv;
}

struct Ring {
    void*         buf = nullptr;
    std::uint32_t cnt = 0;   // entries; a power of two, checked below
    std::uint32_t size = 0;  // 64 or 128 bytes per entry
    const void*   dbrec = nullptr;
    bool          usable = false;
};

struct Stats {
    std::uint64_t polls = 0, completions = 0, read = 0;
    std::uint64_t qpn_ok = 0, len_checked = 0, len_ok = 0, class_ok = 0;
    std::uint64_t recv = 0, recv_with_srqn = 0;
    std::uint32_t wqe_lo = 0xffffffffu, wqe_hi = 0;
    std::uint64_t cqs_seen = 0, cqs_mapped = 0;
    // First few disagreements, verbatim. A rate says how often; these say what.
    std::uint64_t shown = 0;
};

inline std::mutex& mu() {
    static std::mutex m;
    return m;
}
inline std::unordered_map<const void*, Ring>& rings() {
    static auto* m = new std::unordered_map<const void*, Ring>();  // leaked: outlives atexit
    return *m;
}
inline Stats& stats() {
    static auto* s = new Stats();  // leaked, same reason
    return *s;
}

inline int rank_hint() {
    for (const char* v : {"SLURM_PROCID", "OMPI_COMM_WORLD_RANK", "PMI_RANK"})
        if (const char* e = std::getenv(v))
            return std::atoi(e);
    return -1;
}

inline void report() {
    std::lock_guard<std::mutex> lk(mu());
    const Stats&                s = stats();
    const int                   r = rank_hint();
    if (s.completions == 0 && s.cqs_seen == 0)
        return;
    std::fprintf(stderr,
                 "[pcomm-cqeprobe] rank=%d cqs=%llu mapped=%llu polls=%llu completions=%llu\n", r,
                 (unsigned long long)s.cqs_seen, (unsigned long long)s.cqs_mapped,
                 (unsigned long long)s.polls, (unsigned long long)s.completions);
    std::fprintf(
        stderr, "[pcomm-cqeprobe] rank=%d read=%llu qpn_ok=%llu class_ok=%llu len_ok=%llu/%llu\n",
        r, (unsigned long long)s.read, (unsigned long long)s.qpn_ok, (unsigned long long)s.class_ok,
        (unsigned long long)s.len_ok, (unsigned long long)s.len_checked);
    if (s.recv)
        std::fprintf(
            stderr, "[pcomm-cqeprobe] rank=%d recv=%llu with_srqn=%llu wqe_counter=[%u,%u]\n", r,
            (unsigned long long)s.recv, (unsigned long long)s.recv_with_srqn, s.wqe_lo, s.wqe_hi);
    std::fflush(stderr);
}

// The ring for one CQ, resolved once. A CQ that is not mlx5 (or a libmlx5 that
// refuses) is marked unusable and never asked again.
inline const Ring& ring_for(ibv_cq* cq) {
    auto it = rings().find(cq);
    if (it != rings().end())
        return it->second;

    Ring r{};
    // RTLD_DEFAULT IS NOT ENOUGH, and job 44542213 is why.
    //
    // libibverbs loads its hardware providers with dlopen and RTLD_LOCAL, so
    // libmlx5's symbols are NOT in the global search scope of a process that
    // did not link it itself. Under perftest the lookup succeeded — that binary
    // pulls libmlx5 in directly — and under NVSHMEM it returned null on every
    // rank: `mapped=0` on two processes that were otherwise polling 12,348
    // completions through mlx5 hardware. The probe reported "cannot reach the
    // CQ" and it would have been read as "the CQE is not there".
    //
    // So ask libmlx5 itself. RTLD_NOLOAD first, because by the time any CQ
    // exists the provider is certainly loaded and we want ITS handle rather
    // than a second mapping; the plain dlopen is the fallback for the case
    // where it somehow is not.
    static auto* init_obj = [] {
        void* h = dlopen("libmlx5.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = dlopen("libmlx5.so.1", RTLD_NOW);
        void* sym = h ? dlsym(h, "mlx5dv_init_obj") : nullptr;
        if (!sym)  // last resort: a binary that did link it
            sym = dlsym(RTLD_DEFAULT, "mlx5dv_init_obj");
        std::fprintf(stderr, "[pcomm-cqeprobe] mlx5dv_init_obj via %s = %p\n",
                     h ? "libmlx5.so.1" : "RTLD_DEFAULT", sym);
        std::fflush(stderr);
        return reinterpret_cast<int (*)(mlx5dv_obj*, std::uint64_t)>(sym);
    }();
    stats().cqs_seen++;
    if (init_obj) {
        mlx5dv_cq  out{};
        mlx5dv_obj obj{};
        obj.cq.in = cq;
        obj.cq.out = &out;
        if (init_obj(&obj, MLX5DV_OBJ_CQ) == 0 && out.buf && out.cqe_cnt) {
            r.buf = out.buf;
            r.cnt = out.cqe_cnt;
            r.size = out.cqe_size;
            r.dbrec = out.dbrec;
            // Power of two, because the index is masked and not divided — the
            // same thing libmlx5 assumes. If it ever is not, refuse rather than
            // read the wrong slot.
            r.usable = (r.cnt & (r.cnt - 1)) == 0 && (r.size == 64 || r.size == 128) && r.dbrec;
            if (r.usable)
                stats().cqs_mapped++;
            else
                std::fprintf(stderr, "[pcomm-cqeprobe] cq %p unusable: cnt=%u size=%u dbrec=%p\n",
                             (void*)cq, r.cnt, r.size, (void*)r.dbrec);
        }
    } else {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr, "[pcomm-cqeprobe] mlx5dv_init_obj not resolvable\n");
        }
    }
    return rings().emplace(cq, r).first->second;
}

// --- the production path ----------------------------------------------------

// The CQE for completion `i` of a batch of `ret`, or null when this CQ's ring
// could not be mapped. Callers hold mu().
const mlx5_cqe64* cqe_at(ibv_cq* cq, int ret, int i) {
    const Ring& r = ring_for(cq);
    if (!r.usable)
        return nullptr;
    const std::uint32_t ci =
        be32toh(*reinterpret_cast<const volatile std::uint32_t*>(r.dbrec)) & 0xffffffu;
    const std::uint32_t idx = (ci - static_cast<std::uint32_t>(ret) + i) & (r.cnt - 1);
    const auto*         slot = static_cast<const std::uint8_t*>(r.buf) + idx * r.size;
    return reinterpret_cast<const mlx5_cqe64*>(r.size == 64 ? slot : slot + 64);
}

RecvCqe read_recv_cqe(ibv_cq* cq, int ret, int i, const ibv_wc& wc) {
    if (!cq || ret <= 0 || i < 0 || i >= ret)
        return {};
    std::lock_guard<std::mutex> lk(mu());
    const mlx5_cqe64*           cqe = cqe_at(cq, ret, i);
    if (!cqe)
        return {};

    // The SAME cross-check the probe measured, now load-bearing rather than
    // reported. 28,814 CQEs across two NIC generations agreed; this is what
    // makes the 28,815th one that does not agree produce no row instead of a
    // wrong one.
    const std::uint32_t qpn = be32toh(cqe->sop_drop_qpn) & 0xffffffu;
    const std::uint8_t  op = static_cast<std::uint8_t>(cqe->op_own >> 4);
    if (qpn != wc.qp_num || !is_responder(op) || be32toh(cqe->byte_cnt) != wc.byte_len)
        return {};

    RecvCqe out;
    out.srqn = be32toh(cqe->srqn_uidx) & 0xffffffu;
    out.wqe_counter = be16toh(cqe->wqe_counter);
    out.valid = true;
    return out;
}

SrqSlot srq_slot(ibv_srq* srq, unsigned n) {
    if (!srq)
        return {};
    static auto* init_obj = reinterpret_cast<int (*)(mlx5dv_obj*, std::uint64_t)>([] {
        void* h = dlopen("libmlx5.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = dlopen("libmlx5.so.1", RTLD_NOW);
        void* sym = h ? dlsym(h, "mlx5dv_init_obj") : nullptr;
        return sym ? sym : dlsym(RTLD_DEFAULT, "mlx5dv_init_obj");
    }());
    if (!init_obj)
        return {};

    mlx5dv_srq out{};
    // srqn IS OPTIONAL OUTPUT AND MUST BE ASKED FOR. mlx5dv_get_srq fills it
    // only when the caller sets this bit on the way in; without it the field is
    // left at whatever the struct was initialised to. Zero-initialising the
    // struct — the careful-looking thing — turns that into a plausible SRQ
    // number rather than an obvious error, and job 44542534 recorded 32,828
    // slot rows all claiming srqn=0 while the CQEs beside them reported 3 and
    // 4. The join matched nothing, which is the good outcome: it failed loudly
    // instead of attributing bytes through a wrong key.
    out.comp_mask = MLX5DV_SRQ_MASK_SRQN;
    mlx5dv_obj obj{};
    obj.srq.in = srq;
    obj.srq.out = &out;
    if (init_obj(&obj, MLX5DV_OBJ_SRQ) != 0 || !out.buf || !out.stride)
        return {};
    // A capture whose slots all say srqn=0 cannot be joined to a completion, so
    // refuse rather than fill a stream with rows nothing can use.
    if (!(out.comp_mask & MLX5DV_SRQ_MASK_SRQN) || out.srqn == 0)
        return {};

    // A POST CHAIN DOES NOT OCCUPY CONSECUTIVE SLOTS. libmlx5 keeps the SRQ's
    // free WQEs as a linked list and takes the head each time
    // (`srq->head = be16toh(next->next_wqe_index)`), so slot n is found by
    // walking the list n times and not by adding n. Assuming otherwise would
    // put every buffer after the first in a chain at the wrong index — and
    // silently, because the index would still be a plausible one.
    //
    // The link lives in the first 4 bytes of each WQE:
    //   struct mlx5_wqe_srq_next_seg { uint16_t rsvd0; __be16 next_wqe_index; ... }
    // Read here rather than named from a vendored header because those four
    // bytes are all of it, and third_party/mlx5 carries the PRM structs, not
    // libmlx5's internal ones.
    std::uint32_t slot = out.head;
    for (unsigned k = 0; k < n; ++k) {
        const auto* wqe = static_cast<const std::uint8_t*>(out.buf) + slot * out.stride;
        slot = be16toh(*reinterpret_cast<const std::uint16_t*>(wqe + 2));
    }
    return {out.srqn, slot, true};
}

// One poll's worth of completions, checked against the CQEs they came from.
void on_poll(ibv_cq* cq, int ret, ibv_wc* wc) {
    if (!enabled() || ret <= 0 || !wc || !cq)
        return;
    std::lock_guard<std::mutex> lk(mu());

    static bool once = [] {
        std::atexit(report);
        std::fprintf(stderr, "[pcomm-cqeprobe] enabled\n");
        std::fflush(stderr);
        return true;
    }();
    (void)once;

    Stats& s = stats();
    s.polls++;
    s.completions += static_cast<std::uint64_t>(ret);

    const Ring& r = ring_for(cq);
    if (!r.usable)
        return;

    // The consumer index the driver published on its way out of poll_cq. The
    // batch we were handed is the `ret` entries ending there.
    const std::uint32_t ci =
        be32toh(*reinterpret_cast<const volatile std::uint32_t*>(r.dbrec)) & 0xffffffu;
    const std::uint32_t mask = r.cnt - 1;

    for (int i = 0; i < ret; ++i) {
        const std::uint32_t idx = (ci - static_cast<std::uint32_t>(ret) + i) & mask;
        const auto*         slot = static_cast<const std::uint8_t*>(r.buf) + idx * r.size;
        const auto* cqe = reinterpret_cast<const mlx5_cqe64*>(r.size == 64 ? slot : slot + 64);
        s.read++;

        const std::uint32_t qpn = be32toh(cqe->sop_drop_qpn) & 0xffffffu;
        const std::uint8_t  op = static_cast<std::uint8_t>(cqe->op_own >> 4);
        const std::uint32_t len = be32toh(cqe->byte_cnt);
        const bool          wc_is_recv = wc[i].opcode & IBV_WC_RECV;

        const bool qpn_ok = qpn == wc[i].qp_num;
        const bool class_ok = is_responder(op) == wc_is_recv;
        s.qpn_ok += qpn_ok;
        s.class_ok += class_ok;
        if (wc_is_recv) {
            s.len_checked++;
            s.len_ok += (len == wc[i].byte_len);
            s.recv++;
            const std::uint32_t wqe = be16toh(cqe->wqe_counter);
            if (wqe < s.wqe_lo)
                s.wqe_lo = wqe;
            if (wqe > s.wqe_hi)
                s.wqe_hi = wqe;
            if (be32toh(cqe->srqn_uidx) & 0xffffffu)
                s.recv_with_srqn++;
        }

        // The first few rows in full, agreeing or not. A rate tells you how
        // often the arithmetic is right; only the row tells you what it read.
        if (s.shown < 12) {
            s.shown++;
            std::fprintf(stderr,
                         "[pcomm-cqeprobe] rank=%d cqe idx=%u ci=%u cqe_op=%u qpn=%u/%u "
                         "byte=%u/%u wr_id=%llu wqe_counter=%u srqn=%u %s%s\n",
                         rank_hint(), idx, ci, op, qpn, wc[i].qp_num, len, wc[i].byte_len,
                         (unsigned long long)wc[i].wr_id, (unsigned)be16toh(cqe->wqe_counter),
                         (unsigned)(be32toh(cqe->srqn_uidx) & 0xffffffu),
                         qpn_ok ? "QPN-OK" : "QPN-MISMATCH", class_ok ? " CLASS-OK" : " CLASS-BAD");
            std::fflush(stderr);
        }
    }
}

#endif  // PCOMM_HAVE_MLX5DV

}  // namespace ibv::cqeprobe
