// pcomm-read — post-processor. Target pipeline: raw per-type streams + intern
// sidecars -> TimeAligner -> columnar Tables -> exporters (Parquet canonical,
// Perfetto derived). Heavy deps (Arrow) are allowed here — off the hot path.
//
// Current stage: relational CSV export with intern resolution (`--csv`), a
// raw per-stream decoder (`--dump`), Perfetto exporters, and a stream
// inventory. The CSVs are the queryable "log" output until the Parquet writer
// lands (they load directly into DuckDB/Polars/pandas).
//
// Every record type is described exactly once, by a Table<R> specialization:
// its table name, its columns, and how one row prints. The CSV export, --dump
// and the known-type inventory are all derived from that list, so a new record
// type is one Table<> entry — and the exports cannot drift apart (they did:
// --dump used to print raw handles where the CSV resolved them).

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/intern_reader.hpp"
#include "pcomm/rec/stream_reader.hpp"
#include "pcomm/rec/version_map.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

#include "ib_records.hpp"  // provider record schemas (reader knows all types)

namespace fs = std::filesystem;
using namespace pcomm;
using namespace pcomm::core::records;
namespace ibr = pcomm::providers::ibverbs::records;
using rec::Stamped;

namespace {

// Stream loading (header validation included) lives in
// pcomm/rec/stream_reader.hpp so it can be tested and so the versioning reader
// uses the same path. It used to be a local template here, where nothing could
// reach it.
using rec::load_stream;

const char* kind_name(core::RecordKind k) {
    switch (k) {
        case core::RecordKind::Event: return "event";
        case core::RecordKind::Entity: return "entity";
        case core::RecordKind::Artifact: return "artifact";
        case core::RecordKind::Edge: return "edge";
    }
    return "?";
}

const char* memop_name(core::MemOp op) {
    switch (op) {
        case core::MemOp::Load: return "load";
        case core::MemOp::Store: return "store";
        case core::MemOp::Atomic: return "atomic";
        case core::MemOp::Prefetch: return "prefetch";
    }
    return "?";
}

// Stable string names for the ibverbs enums, so CSV columns (and the goldens
// that query them) read as physics — "send", "success", "rtr" — instead of
// raw ABI integers, and stay put if the numeric encoding ever changes. Unknown
// values round-trip losslessly as "op<n>" so nothing silently collapses.
std::string wr_opcode_name(unsigned op) {  // ibv_wr_opcode
    switch (op) {
        case 0: return "rdma_write";  case 1: return "rdma_write_imm";
        case 2: return "send";        case 3: return "send_imm";
        case 4: return "rdma_read";   case 5: return "atomic_cmp_swp";
        case 6: return "atomic_fetch_add";
    }
    return "op" + std::to_string(op);
}
std::string wc_opcode_name(unsigned op) {  // ibv_wc_opcode
    switch (op) {
        case 0: return "send";     case 1: return "rdma_write";
        case 2: return "rdma_read"; case 3: return "comp_swap";
        case 4: return "fetch_add"; case 128: return "recv";
        case 129: return "recv_rdma_imm";
    }
    return "wc" + std::to_string(op);
}
std::string wc_status_name(unsigned s) {  // ibv_wc_status (0 = success)
    return s == 0 ? "success" : "err" + std::to_string(s);
}
std::string qp_state_name(unsigned st) {  // ibv_qp_state
    switch (st) {
        case 0: return "reset"; case 1: return "init"; case 2: return "rtr";
        case 3: return "rts";   case 4: return "sqd";  case 5: return "sqe";
        case 6: return "err";
    }
    return "st" + std::to_string(st);
}
std::string qp_type_name(unsigned t) {  // ibv_qp_type
    switch (t) {
        case 2: return "rc"; case 3: return "uc"; case 4: return "ud";
        case 8: return "raw_packet";
    }
    return "qpt" + std::to_string(t);
}

// --- the record descriptor table ---------------------------------------------
// One specialization per exportable record type: table name, column header, and
// THE row printer. `--csv` prefixes each row with the rank; `--dump` prints it
// bare; both call the same row(), so a field's format exists in one place.
template <core::Record R>
struct Table;  // no primary definition: an unspecialized use is a missing entry

// ticks_lo/ticks_hi, not `ticks`: a GPU access is a Window, not an Instant. The
// device has no clock we share, so what is known is that the access happened
// after its kernel was launched and before the host drained it from the NVBit
// channel — see RawAccess in pcomm/attr/interfaces.hpp. Two named columns
// because one called `ticks` was printed identically to ib_post_send's `ticks`,
// which is a *measured* call-entry time; joining the two silently correlated
// drain time against wire time. ticks_lo == 0 means the lower bound is open.
template <>
struct Table<GpuAccess> {
    static constexpr const char* name = "gpu_access";
    static constexpr const char* cols =
        "ticks_lo,ticks_hi,pc,object,addr,size,op,run_dev,mem_dev,thread";
    static void row(std::FILE* f, const Stamped<GpuAccess>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%llu,%u,%u,0x%llx,%u,%s,%u,%u,%u",
                     (unsigned long long)s.temporal.a.ticks,
                     (unsigned long long)s.temporal.b.ticks, s.payload.pc,
                     s.payload.object, (unsigned long long)s.payload.address,
                     s.payload.size, memop_name(s.payload.op),
                     s.payload.run_device, s.payload.mem_device, s.payload.thread);
    }
};

template <>
struct Table<MemObject> {
    static constexpr const char* name = "mem_object";
    static constexpr const char* cols = "ticks,id,name,base,size,device";
    static void row(std::FILE* f, const Stamped<MemObject>& s, const rec::InternDictionary& in) {
        std::fprintf(f, "%llu,%u,%s,0x%llx,%llu,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.id,
                     in.str(s.payload.name),
                     (unsigned long long)s.payload.range.base,
                     (unsigned long long)s.payload.range.size, s.payload.device);
    }
};

template <>
struct Table<SourceLine> {
    static constexpr const char* name = "source_line";
    static constexpr const char* cols = "pc,file,line";
    static void row(std::FILE* f, const Stamped<SourceLine>& s, const rec::InternDictionary& in) {
        std::fprintf(f, "%u,%s,%u", s.payload.pc, in.str(s.payload.file), s.payload.line);
    }
};

template <>
struct Table<ClockAnchor> {
    static constexpr const char* name = "clock_anchor";
    static constexpr const char* cols = "domain,local_ticks,wall_ns,mono_ns";
    static void row(std::FILE* f, const Stamped<ClockAnchor>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%u,%llu,%llu,%llu", s.payload.domain,
                     (unsigned long long)s.payload.local_ticks,
                     (unsigned long long)s.payload.wall_ns,
                     (unsigned long long)s.payload.mono_ns);
    }
};

template <>
struct Table<Attribution> {
    static constexpr const char* name = "attribution";
    static constexpr const char* cols = "ticks,subject,object,pred";
    static void row(std::FILE* f, const Stamped<Attribution>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.subject, s.payload.object,
                     static_cast<unsigned>(s.payload.pred));
    }
};

template <>
struct Table<ibr::IbMemRegion> {
    static constexpr const char* name = "ib_mem_region";
    static constexpr const char* cols = "ticks,mr,pd,base,size,lkey,rkey,access";
    static void row(std::FILE* f, const Stamped<ibr::IbMemRegion>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%llx,%llu,%u,%u,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.mr,
                     s.payload.pd, (unsigned long long)s.payload.range.base,
                     (unsigned long long)s.payload.range.size, s.payload.lkey,
                     s.payload.rkey, s.payload.access);
    }
};

// qp/mr/cq/pd are interned pointer handles (stable within a rank); qp_num /
// dest_qp_num / lid are the wire ids used to stitch the cross-rank comm graph.
template <>
struct Table<ibr::IbQueuePair> {
    static constexpr const char* name = "ib_queue_pair";
    static constexpr const char* cols = "ticks,qp,pd,qp_num,qp_type";
    static void row(std::FILE* f, const Stamped<ibr::IbQueuePair>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%s", (unsigned long long)s.temporal.a.ticks,
                     s.payload.qp, s.payload.pd, s.payload.qp_num,
                     qp_type_name(s.payload.qp_type).c_str());
    }
};

template <>
struct Table<ibr::IbModifyQp> {
    static constexpr const char* name = "ib_modify_qp";
    static constexpr const char* cols = "ticks,qp,qp_num,dest_qp_num,dest_lid,state";
    static void row(std::FILE* f, const Stamped<ibr::IbModifyQp>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%s", (unsigned long long)s.temporal.a.ticks,
                     s.payload.qp, s.payload.qp_num, s.payload.dest_qp_num,
                     s.payload.dest_lid, qp_state_name(s.payload.state).c_str());
    }
};

template <>
struct Table<ibr::IbPostSend> {
    static constexpr const char* name = "ib_post_send";
    static constexpr const char* cols = "ticks,qp,wr_id,remote_addr,rkey,length,qp_num,opcode,num_sge";
    static void row(std::FILE* f, const Stamped<ibr::IbPostSend>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,0x%llx,%u,%u,%u,%s,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                     (unsigned long long)s.payload.wr_id,
                     (unsigned long long)s.payload.remote_addr, s.payload.rkey,
                     s.payload.length, s.payload.qp_num,
                     wr_opcode_name(s.payload.opcode).c_str(), s.payload.num_sge);
    }
};

template <>
struct Table<ibr::IbPollCq> {
    static constexpr const char* name = "ib_poll_cq";
    static constexpr const char* cols = "ticks,cq,wr_id,byte_len,qp_num,status,opcode";
    static void row(std::FILE* f, const Stamped<ibr::IbPollCq>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,%u,%u,%s,%s",
                     (unsigned long long)s.temporal.a.ticks, s.payload.cq,
                     (unsigned long long)s.payload.wr_id, s.payload.byte_len,
                     s.payload.qp_num, wc_status_name(s.payload.status).c_str(),
                     wc_opcode_name(s.payload.opcode).c_str());
    }
};

// Every record type with a Table<> above, in table-emission order. This is the
// list everything iterates; adding a record type means specializing Table<> and
// naming it here — the compiler refuses one without the other.
using TableRecords = std::tuple<GpuAccess, MemObject, SourceLine, ClockAnchor,
                                Attribution, ibr::IbQueuePair, ibr::IbModifyQp,
                                ibr::IbPostSend, ibr::IbPollCq, ibr::IbMemRegion>;

template <typename Tuple, typename F, std::size_t... Is>
void for_each_type_impl(F&& f, std::index_sequence<Is...>) {
    (f.template operator()<std::tuple_element_t<Is, Tuple>>(), ...);
}
template <typename Tuple, typename F>
void for_each_type(F&& f) {
    for_each_type_impl<Tuple>(std::forward<F>(f),
                              std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

// Every record type this build can decode: the tables plus the two intern
// streams (consumed as the dictionary, not exported as tables). Anything else
// in a capture is data we are dropping, and dropping data quietly is the
// failure the record schema exists to prevent — so export_csv reports it rather
// than producing a table that merely looks complete.
std::vector<core::RecordTypeId> known_types() {
    std::vector<core::RecordTypeId> v = {core::RecordTrait<InternKey>::type_id,
                                         core::RecordTrait<InternString>::type_id};
    for_each_type<TableRecords>(
        [&]<typename R>() { v.push_back(core::RecordTrait<R>::type_id); });
    return v;
}

// One rank's capture: streams + intern dictionaries.
struct RankDir {
    int rank{0};
    fs::path dir;
    rec::InternDictionary intern;   // pcomm/rec/intern_reader.hpp

    const char* str(core::Handle h) const { return intern.str(h); }
    const char* str(rec::InternDomain d, core::Handle h) const { return intern.str(d, h); }
};

// A rank's stream file for record type R.
template <core::Record R>
fs::path stream_path(const RankDir& r) { return rec::stream_path<R>(r.dir); }

// Find every rank-* directory under `root`. A single run lays them out flat
// (root/rank-*); a replayed run nests one level per pass (root/<pass>/rank-*),
// e.g. root/ib/rank-0 and root/gpumem/rank-0 for the same logical rank 0. We
// recurse and accept duplicate rank numbers across passes — each RankDir loads
// its own intern table, so pass-local handles never collide, and because each
// provider emits a disjoint record-type range the passes merge by rank number.
void scan_ranks_into(const fs::path& root, std::vector<RankDir>& ranks) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        if (name.rfind("rank-", 0) != 0) {
            scan_ranks_into(entry.path(), ranks);  // descend into a pass subdir
            continue;
        }
        RankDir r;
        r.rank = std::atoi(name.c_str() + 5);
        r.dir = entry.path();
        r.intern = rec::InternDictionary::load(r.dir);
        ranks.push_back(std::move(r));
    }
}

std::vector<RankDir> scan_ranks(const fs::path& root) {
    std::vector<RankDir> ranks;
    scan_ranks_into(root, ranks);
    return ranks;
}

// --- CSV export ---------------------------------------------------------------

// Accumulators for the derived rank-space comm_edges table and the summary
// line. Fed one record at a time from the generic export loop: the overloads
// below pick out the types that carry graph information, the template swallows
// the rest.
struct CsvAgg {
    std::unordered_map<std::uint32_t, int> qp_owner;          // qp_num -> rank
    std::map<std::pair<int, std::uint32_t>, std::uint32_t> rtr_dest;  // (rank,qp_num)->dest_qp_num
    std::map<std::pair<int, std::uint32_t>, std::pair<long, long>> send_by_qp;  // (rank,qp_num)->(sends,bytes)
    std::size_t n_access = 0, n_send = 0;
};

template <core::Record R>
void aggregate(CsvAgg&, const RankDir&, const Stamped<R>&) {}

void aggregate(CsvAgg& a, const RankDir&, const Stamped<GpuAccess>&) { a.n_access++; }

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<ibr::IbQueuePair>& s) {
    a.qp_owner[s.payload.qp_num] = r.rank;
}

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<ibr::IbModifyQp>& s) {
    if (s.payload.state == 2 /*RTR*/ && s.payload.dest_qp_num)
        a.rtr_dest[{r.rank, s.payload.qp_num}] = s.payload.dest_qp_num;
}

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<ibr::IbPostSend>& s) {
    auto& agg = a.send_by_qp[{r.rank, s.payload.qp_num}];
    agg.first += 1;
    agg.second += s.payload.length;
    a.n_send++;
}

int export_csv(const fs::path& root, const fs::path& outdir) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm-read: no rank-* directories under %s\n",
                     root.c_str());
        return 1;
    }
    fs::create_directories(outdir);

    // Say what we cannot represent, before writing tables that omit it.
    std::size_t unhandled = 0;
    for (const auto& r : ranks)
        for (const auto& d : rec::unhandled_streams(r.dir, known_types())) {
            std::fprintf(stderr, "pcomm-read: rank %d: %s\n", r.rank, d.message.c_str());
            ++unhandled;
        }
    if (unhandled)
        std::fprintf(stderr,
                     "pcomm-read: %zu stream(s) not exported — this build is older "
                     "than the capture, or was built without a provider it used\n",
                     unhandled);

    // One <name>.csv per Table<>, each row prefixed with the rank.
    std::unordered_map<core::RecordTypeId, std::FILE*> files;
    for_each_type<TableRecords>([&]<typename R>() {
        std::FILE* f = std::fopen((outdir / (std::string(Table<R>::name) + ".csv")).c_str(), "w");
        std::fprintf(f, "rank,%s\n", Table<R>::cols);
        files[core::RecordTrait<R>::type_id] = f;
    });

    CsvAgg agg;
    for (const auto& r : ranks) {
        for_each_type<TableRecords>([&]<typename R>() {
            std::FILE* f = files[core::RecordTrait<R>::type_id];
            for (const auto& s : load_stream<R>(stream_path<R>(r))) {
                std::fprintf(f, "%d,", r.rank);
                Table<R>::row(f, s, r.intern);
                std::fputc('\n', f);
                aggregate(agg, r, s);
            }
        });
    }

    // Derived: rank-space communication graph. Each rank's post_sends on a QP
    // reach the peer that owns that QP's RTR dest_qp_num — collapsing wire ids
    // (qp_num/LID, which differ every run) to a stable src_rank->dst_rank shape
    // with total sends and bytes. This is the table topology assertions query,
    // so tests never touch run-specific identifiers.
    //
    // NB qp_num is HCA-local, so across nodes two QPs can share a number; the
    // owner map is exact for a single HCA / few ranks (pingpong, one node) and
    // approximate at multinode scale until the local LID is captured directly
    // (things-to-circle-back-to.md). Deterministic either way → golden-safe.
    std::FILE* edges = std::fopen((outdir / "comm_edges.csv").c_str(), "w");
    std::fputs("src_rank,dst_rank,sends,bytes\n", edges);
    std::map<std::pair<int, int>, std::pair<long, long>> edge_agg;
    for (const auto& [key, sb] : agg.send_by_qp) {
        auto [src_rank, qp_num] = key;
        int dst_rank = -1;
        if (auto it = agg.rtr_dest.find({src_rank, qp_num}); it != agg.rtr_dest.end())
            if (auto o = agg.qp_owner.find(it->second); o != agg.qp_owner.end())
                dst_rank = o->second;
        auto& e = edge_agg[{src_rank, dst_rank}];
        e.first += sb.first;
        e.second += sb.second;
    }
    for (const auto& [rk, e] : edge_agg)
        std::fprintf(edges, "%d,%d,%ld,%ld\n", rk.first, rk.second, e.first, e.second);
    std::fclose(edges);

    for (const auto& [_, f] : files) std::fclose(f);

    std::printf("pcomm-read: %zu ranks, %zu gpu-accesses, %zu ib-sends -> %s/"
                "{gpu_access,mem_object,source_line,clock_anchor,comm_edges,ib_*}.csv\n",
                ranks.size(), agg.n_access, agg.n_send, outdir.c_str());
    return 0;
}

// --- rank clock calibration ---------------------------------------------------
// TSC ticks aren't comparable across nodes (per-CPU, boot-relative), so each
// rank's ClockAnchors (start+end: local_ticks + wall_ns) calibrate ticks ->
// wall-ns; a shared wall-ns origin keeps µs precision and aligns all ranks on
// one axis. Used identically by both Perfetto exporters (it was copy-pasted
// between them once, byte for byte).
struct RankClock {
    long double t0{0}, w0{0};   // anchor tick / wall_ns
    double ns_per_tick{1.0};
    std::uint64_t wall_at(std::uint64_t tick) const {
        return static_cast<std::uint64_t>(w0 + (static_cast<long double>(tick) - t0) * ns_per_tick);
    }
    // An Interval's duration in ns; 0 for instants (and for backwards intervals,
    // which a broken capture could contain).
    //
    // Deliberately Interval *only*. A Window also has both endpoints populated,
    // but they bound uncertainty rather than measuring a duration — drawing one
    // as a slice would assert the event took that long, which is exactly the
    // false claim this distinction exists to prevent. Nothing exports Windows
    // to a timeline today (gpu accesses go to CSV, not Perfetto); when
    // something does, it needs its own rendering, not this.
    double dur_ns(const core::Temporal& t) const {
        return (t.kind == core::TimeKind::Interval && t.b.ticks > t.a.ticks)
                   ? (t.b.ticks - t.a.ticks) * ns_per_tick
                   : 0.0;
    }
};

// Fits one RankClock per rank and picks the earliest anchored wall-ns as the
// shared origin (0 if no rank has anchors).
std::pair<std::vector<RankClock>, std::uint64_t>
build_clocks(const std::vector<RankDir>& ranks) {
    std::vector<RankClock> clk(ranks.size());
    std::uint64_t origin = ~0ull;
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        auto anchors = load_stream<ClockAnchor>(stream_path<ClockAnchor>(ranks[i]));
        if (!anchors.empty()) {
            const auto& a = anchors.front().payload;
            clk[i].t0 = a.local_ticks; clk[i].w0 = a.wall_ns;
            if (anchors.size() >= 2) {
                const auto& b = anchors.back().payload;
                long double dt = static_cast<long double>(b.local_ticks) - a.local_ticks;
                if (dt > 0)
                    clk[i].ns_per_tick =
                        static_cast<double>((static_cast<long double>(b.wall_ns) - a.wall_ns) / dt);
            }
            origin = std::min(origin, static_cast<std::uint64_t>(a.wall_ns));
        }
    }
    if (origin == ~0ull) origin = 0;
    return {std::move(clk), origin};
}

// --- Perfetto / Chrome JSON trace --------------------------------------------
// One process per rank, one thread per QP; post_send/poll_cq become slices
// (instants when the record carries no interval). Load in https://ui.perfetto.dev.
int export_perfetto(const fs::path& root, const fs::path& outfile) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm-read: no rank-* directories under %s\n", root.c_str());
        return 1;
    }
    auto [clk, origin] = build_clocks(ranks);

    std::FILE* f = std::fopen(outfile.c_str(), "w");
    if (!f) { std::fprintf(stderr, "pcomm-read: cannot write %s\n", outfile.c_str()); return 1; }
    std::fputs("{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n", f);
    bool first = true;
    auto sep = [&] { if (!first) std::fputs(",\n", f); first = false; };
    auto us = [&](const RankClock& c, std::uint64_t tick) {
        return static_cast<double>(c.wall_at(tick) - origin) / 1000.0;  // ns -> µs
    };

    std::size_t n_ev = 0;
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        const auto& r = ranks[i];
        const auto& c = clk[i];
        sep();
        std::fprintf(f, "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":%d,"
                        "\"args\":{\"name\":\"rank %d\"}}", r.rank, r.rank);
        std::unordered_map<std::uint32_t, bool> seen;
        for (const auto& s : load_stream<ibr::IbQueuePair>(stream_path<ibr::IbQueuePair>(r))) {
            if (seen.emplace(s.payload.qp_num, true).second) {
                sep();
                std::fprintf(f, "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":%d,\"tid\":%u,"
                                "\"args\":{\"name\":\"QP 0x%x\"}}", r.rank, s.payload.qp_num,
                             s.payload.qp_num);
            }
        }
        for (const auto& s : load_stream<ibr::IbPostSend>(stream_path<ibr::IbPostSend>(r))) {
            sep();
            double ts = us(c, s.temporal.a.ticks), d = c.dur_ns(s.temporal) / 1000.0;
            if (d > 0)
                std::fprintf(f, "{\"ph\":\"X\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f,"
                                "\"name\":\"send %uB\",\"args\":{\"opcode\":%u,\"bytes\":%u,\"wr\":%llu}}",
                             r.rank, s.payload.qp_num, ts, d, s.payload.length, s.payload.opcode,
                             s.payload.length, (unsigned long long)s.payload.wr_id);
            else
                std::fprintf(f, "{\"ph\":\"i\",\"s\":\"t\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,"
                                "\"name\":\"send %uB\",\"args\":{\"opcode\":%u,\"bytes\":%u,\"wr\":%llu}}",
                             r.rank, s.payload.qp_num, ts, s.payload.length, s.payload.opcode,
                             s.payload.length, (unsigned long long)s.payload.wr_id);
            n_ev++;
        }
        for (const auto& s : load_stream<ibr::IbPollCq>(stream_path<ibr::IbPollCq>(r))) {
            sep();
            double ts = us(c, s.temporal.a.ticks), d = c.dur_ns(s.temporal) / 1000.0;
            if (d > 0)
                std::fprintf(f, "{\"ph\":\"X\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f,"
                                "\"name\":\"complete\",\"args\":{\"status\":%u,\"opcode\":%u,\"bytes\":%u}}",
                             r.rank, s.payload.qp_num, ts, d, s.payload.status, s.payload.opcode,
                             s.payload.byte_len);
            else
                std::fprintf(f, "{\"ph\":\"i\",\"s\":\"t\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,"
                                "\"name\":\"complete\",\"args\":{\"status\":%u,\"opcode\":%u,\"bytes\":%u}}",
                             r.rank, s.payload.qp_num, ts, s.payload.status, s.payload.opcode,
                             s.payload.byte_len);
            n_ev++;
        }
    }
    std::fputs("\n]}\n", f);
    std::fclose(f);
    std::printf("pcomm-read: %zu ranks, %zu events -> %s (load in ui.perfetto.dev)\n",
                ranks.size(), n_ev, outfile.c_str());
    return 0;
}

// --- Perfetto native protobuf trace (.pftrace) -------------------------------
// Canonical Perfetto format: a length-delimited stream of TracePacket (Trace
// field 1). We emit TrackDescriptors (one track per QP, parented to a per-rank
// track) and TrackEvents (SLICE_BEGIN/END). Field numbers are from Perfetto's
// trace_packet.proto / track_event.proto. Validate structure with
// `protoc --decode_raw < out.pftrace`.
struct PB {
    std::string b;
    void varint(std::uint64_t v) {
        while (v >= 0x80) { b.push_back(static_cast<char>(v | 0x80)); v >>= 7; }
        b.push_back(static_cast<char>(v));
    }
    void tag(int field, int wire) { varint((static_cast<std::uint64_t>(field) << 3) | wire); }
    void u64(int field, std::uint64_t v) { tag(field, 0); varint(v); }
    void str(int field, const std::string& s) { tag(field, 2); varint(s.size()); b += s; }
    void msg(int field, const PB& m) { tag(field, 2); varint(m.b.size()); b += m.b; }
};

int export_pftrace(const fs::path& root, const fs::path& outfile) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm-read: no rank-* directories under %s\n", root.c_str());
        return 1;
    }
    auto [clk, origin] = build_clocks(ranks);

    constexpr std::uint64_t SEQ = 1;
    auto rank_uuid = [](int rank) { return static_cast<std::uint64_t>(rank) + 1; };
    auto qp_uuid = [](int rank, std::uint32_t qp) {
        return (static_cast<std::uint64_t>(rank) + 1) * 0x100000000ull + qp;
    };
    auto ns = [&](const RankClock& c, std::uint64_t tick) {
        return c.wall_at(tick) - origin;
    };

    PB trace;
    auto track = [&](std::uint64_t uuid, const std::string& name, std::uint64_t parent) {
        PB td; td.u64(1, uuid); td.str(2, name);
        if (parent) td.u64(5, parent);
        PB pkt; pkt.msg(60, td); pkt.u64(10, SEQ);
        trace.msg(1, pkt);
    };
    auto slice = [&](std::uint64_t track_uuid, std::uint64_t t_ns, std::uint64_t dur_ns,
                     const std::string& name) {
        PB beg; beg.u64(9, dur_ns ? 1u : 3u); beg.u64(11, track_uuid); beg.str(23, name);  // BEGIN|INSTANT
        PB p1; p1.u64(8, t_ns); p1.msg(11, beg); p1.u64(10, SEQ); trace.msg(1, p1);
        if (dur_ns) {
            PB end; end.u64(9, 2u); end.u64(11, track_uuid);  // SLICE_END
            PB p2; p2.u64(8, t_ns + dur_ns); p2.msg(11, end); p2.u64(10, SEQ); trace.msg(1, p2);
        }
    };

    std::size_t n_ev = 0;
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        const auto& r = ranks[i];
        const auto& c = clk[i];
        track(rank_uuid(r.rank), "rank " + std::to_string(r.rank), 0);
        std::unordered_map<std::uint32_t, bool> seen;
        for (const auto& s : load_stream<ibr::IbQueuePair>(stream_path<ibr::IbQueuePair>(r))) {
            if (seen.emplace(s.payload.qp_num, true).second) {
                char nm[24]; std::snprintf(nm, sizeof nm, "QP 0x%x", s.payload.qp_num);
                track(qp_uuid(r.rank, s.payload.qp_num), nm, rank_uuid(r.rank));
            }
        }
        for (const auto& s : load_stream<ibr::IbPostSend>(stream_path<ibr::IbPostSend>(r))) {
            char nm[24]; std::snprintf(nm, sizeof nm, "send %uB", s.payload.length);
            slice(qp_uuid(r.rank, s.payload.qp_num), ns(c, s.temporal.a.ticks),
                  static_cast<std::uint64_t>(c.dur_ns(s.temporal)), nm);
            n_ev++;
        }
        for (const auto& s : load_stream<ibr::IbPollCq>(stream_path<ibr::IbPollCq>(r))) {
            slice(qp_uuid(r.rank, s.payload.qp_num), ns(c, s.temporal.a.ticks),
                  static_cast<std::uint64_t>(c.dur_ns(s.temporal)), "complete");
            n_ev++;
        }
    }

    std::FILE* f = std::fopen(outfile.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "pcomm-read: cannot write %s\n", outfile.c_str()); return 1; }
    std::fwrite(trace.b.data(), 1, trace.b.size(), f);
    std::fclose(f);
    std::printf("pcomm-read: %zu ranks, %zu events -> %s (%zu bytes; load in ui.perfetto.dev)\n",
                ranks.size(), n_ev, outfile.c_str(), trace.b.size());
    return 0;
}

// --- raw single-stream dump ---------------------------------------------------
// Same Table<> printers as the CSV export, minus the rank column. The intern
// dictionary is loaded from the stream's own rank directory, so resolved fields
// (object names, file paths) read the same here as in the CSV — they used to be
// raw handles here, which is exactly the drift the shared printers close off.
void dump(core::RecordTypeId type, const fs::path& file) {
    bool matched = false;
    for_each_type<TableRecords>([&]<typename R>() {
        if (core::RecordTrait<R>::type_id != type || matched) return;
        matched = true;
        const auto intern = rec::InternDictionary::load(file.parent_path());
        std::puts(Table<R>::cols);
        for (const auto& s : load_stream<R>(file)) {
            Table<R>::row(stdout, s, intern);
            std::fputc('\n', stdout);
        }
    });
    if (!matched)
        std::fprintf(stderr, "pcomm-read: no decoder for type %u yet\n", type);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(
            "usage: pcomm-read <trace-dir>                    stream inventory\n"
            "       pcomm-read <trace-dir> --csv <outdir>     export all tables (intern-resolved)\n"
            "       pcomm-read <trace-dir> --perfetto <out>   export Perfetto/Chrome JSON trace\n"
            "       pcomm-read <trace-dir> --pftrace <out>    export Perfetto native protobuf trace\n"
            "       pcomm-read <trace-dir> --dump <typeid>    decode one raw stream\n",
            stderr);
        return 1;
    }

    fs::path dir = argv[1];
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm-read: not a directory: %s\n", dir.c_str());
        return 1;
    }

    if (argc >= 4 && std::strcmp(argv[2], "--csv") == 0) {
        return export_csv(dir, argv[3]);
    }

    if (argc >= 4 && std::strcmp(argv[2], "--perfetto") == 0) {
        return export_perfetto(dir, argv[3]);
    }

    if (argc >= 4 && std::strcmp(argv[2], "--pftrace") == 0) {
        return export_pftrace(dir, argv[3]);
    }

    if (argc >= 4 && std::strcmp(argv[2], "--dump") == 0) {
        auto type = static_cast<core::RecordTypeId>(std::atoi(argv[3]));
        bool found = false;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() &&
                entry.path().filename() == std::to_string(type) + ".pcs") {
                dump(type, entry.path());
                found = true;
            }
        }
        if (!found) {
            std::fprintf(stderr, "pcomm-read: no %u.pcs under %s\n", type, dir.c_str());
            return 1;
        }
        return 0;
    }

    // The inventory reads each file's own header rather than inferring anything
    // from its name or its size. This is the first thing in the tree that is
    // told what a capture is by the capture itself.
    std::printf("streams in %s:\n", dir.c_str());
    std::printf("  %-34s %6s %-9s %7s %9s %16s\n",
                "file", "type", "kind", "stride", "records", "schema");
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pcs") continue;

        const auto rel = fs::relative(entry.path(), dir);
        const auto bytes = static_cast<std::uintmax_t>(entry.file_size());
        rec::StreamHeader h{};
        std::ifstream in(entry.path(), std::ios::binary);
        if (bytes < sizeof h || !in.read(reinterpret_cast<char*>(&h), sizeof h) ||
            h.magic != rec::kStreamMagic) {
            std::printf("  %-34s %10ju bytes  (no stream header — pre-format-1 "
                        "capture, re-capture it)\n", rel.c_str(), bytes);
            continue;
        }
        const auto payload = bytes - sizeof h;
        std::printf("  %-34s %6u %-9s %7u %9ju %016llx%s\n", rel.c_str(), h.type_id,
                    kind_name(h.kind), h.stride,
                    h.stride ? static_cast<std::uintmax_t>(payload / h.stride) : 0,
                    (unsigned long long)h.schema_hash,
                    h.schema_hash == rec::kSchemaHash ? "" : " (other schema)");
        if (h.stride && payload % h.stride)
            std::printf("  %-34s   ^ truncated: %ju trailing bytes are not a whole record\n",
                        "", static_cast<std::uintmax_t>(payload % h.stride));
    }
    return 0;
}
