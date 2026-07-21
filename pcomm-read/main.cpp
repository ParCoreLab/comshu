// pcomm-read — post-processor. Target pipeline: raw per-type streams + intern
// sidecars -> TimeAligner -> columnar Tables -> exporters (Parquet canonical,
// Perfetto derived). Heavy deps (Arrow) are allowed here — off the hot path.
//
// Current stage: relational CSV export with intern resolution (`--csv`), a
// raw per-stream decoder (`--dump`), and a stream inventory. The CSVs are the
// queryable "log" output until the Parquet writer lands (they load directly
// into DuckDB/Polars/pandas). The per-type decode functions become a
// trait-driven generic table builder when the exporter framework lands.

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"

#include "ib_records.hpp"  // provider record schemas (reader knows all types)

namespace fs = std::filesystem;
using namespace pcomm;
using namespace pcomm::core::records;
namespace ibr = pcomm::providers::ibverbs::records;
using rec::Stamped;

namespace {

template <core::Record R>
std::vector<Stamped<R>> load_stream(const fs::path& file) {
    std::vector<Stamped<R>> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    auto bytes = fs::file_size(file);
    out.resize(bytes / sizeof(Stamped<R>));
    in.read(reinterpret_cast<char*>(out.data()),
            static_cast<std::streamsize>(out.size() * sizeof(Stamped<R>)));
    return out;
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

// One rank's capture: streams + intern dictionaries.
struct RankDir {
    int rank{0};
    fs::path dir;
    std::unordered_map<core::Handle, std::string> strings;  // intern-string.tsv

    const char* str(core::Handle h) const {
        auto it = strings.find(h);
        return it == strings.end() ? "" : it->second.c_str();
    }
};

std::vector<RankDir> scan_ranks(const fs::path& root) {
    std::vector<RankDir> ranks;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        if (name.rfind("rank-", 0) != 0) continue;
        RankDir r;
        r.rank = std::atoi(name.c_str() + 5);
        r.dir = entry.path();
        std::ifstream in(r.dir / "intern-string.tsv");
        std::string line;
        while (std::getline(in, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            r.strings.emplace(static_cast<core::Handle>(std::stoul(line.substr(0, tab))),
                              line.substr(tab + 1));
        }
        ranks.push_back(std::move(r));
    }
    return ranks;
}

template <core::Record R>
fs::path stream_path(const RankDir& r) {
    return r.dir / (std::to_string(core::RecordTrait<R>::type_id) + ".pcs");
}

// --- CSV export ---------------------------------------------------------------

int export_csv(const fs::path& root, const fs::path& outdir) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm-read: no rank-* directories under %s\n",
                     root.c_str());
        return 1;
    }
    fs::create_directories(outdir);

    auto open = [&](const char* name, const char* header) {
        std::FILE* f = std::fopen((outdir / name).c_str(), "w");
        std::fputs(header, f);
        return f;
    };

    std::FILE* access = open("gpu_access.csv",
        "rank,ticks,pc,object,addr,size,op,run_dev,mem_dev,thread\n");
    std::FILE* object = open("mem_object.csv",
        "rank,ticks,id,name,base,size,device\n");
    std::FILE* source = open("source_line.csv", "rank,pc,file,line\n");
    std::FILE* anchor = open("clock_anchor.csv",
        "rank,domain,local_ticks,wall_ns,mono_ns\n");

    // ibverbs join tables (network side). qp/mr/cq/pd are interned pointer
    // handles (stable within a rank); qp_num/dest_qp_num/lid are the wire ids
    // used to stitch the cross-rank comm graph.
    std::FILE* ib_qp = open("ib_queue_pair.csv", "rank,ticks,qp,pd,qp_num,qp_type\n");
    std::FILE* ib_mod = open("ib_modify_qp.csv",
        "rank,ticks,qp,qp_num,dest_qp_num,dest_lid,state\n");
    std::FILE* ib_send = open("ib_post_send.csv",
        "rank,ticks,qp,wr_id,remote_addr,rkey,length,qp_num,opcode,num_sge\n");
    std::FILE* ib_cq = open("ib_poll_cq.csv",
        "rank,ticks,cq,wr_id,byte_len,qp_num,status,opcode\n");
    std::FILE* ib_mr = open("ib_mem_region.csv",
        "rank,ticks,mr,pd,base,size,lkey,rkey,access\n");

    std::size_t n_access = 0, n_send = 0;
    for (const auto& r : ranks) {
        for (const auto& s : load_stream<GpuAccess>(stream_path<GpuAccess>(r))) {
            std::fprintf(access, "%d,%llu,%u,%u,0x%llx,%u,%s,%u,%u,%u\n", r.rank,
                         (unsigned long long)s.temporal.a.ticks, s.payload.pc,
                         s.payload.object, (unsigned long long)s.payload.address,
                         s.payload.size, memop_name(s.payload.op),
                         s.payload.run_device, s.payload.mem_device,
                         s.payload.thread);
            n_access++;
        }
        for (const auto& s : load_stream<MemObject>(stream_path<MemObject>(r))) {
            std::fprintf(object, "%d,%llu,%u,%s,0x%llx,%llu,%u\n", r.rank,
                         (unsigned long long)s.temporal.a.ticks, s.payload.id,
                         r.str(s.payload.name),
                         (unsigned long long)s.payload.range.base,
                         (unsigned long long)s.payload.range.size,
                         s.payload.device);
        }
        for (const auto& s : load_stream<SourceLine>(stream_path<SourceLine>(r))) {
            std::fprintf(source, "%d,%u,%s,%u\n", r.rank, s.payload.pc,
                         r.str(s.payload.file), s.payload.line);
        }
        for (const auto& s : load_stream<ClockAnchor>(stream_path<ClockAnchor>(r))) {
            std::fprintf(anchor, "%d,%u,%llu,%llu,%llu\n", r.rank, s.payload.domain,
                         (unsigned long long)s.payload.local_ticks,
                         (unsigned long long)s.payload.wall_ns,
                         (unsigned long long)s.payload.mono_ns);
        }
        for (const auto& s : load_stream<ibr::IbQueuePair>(stream_path<ibr::IbQueuePair>(r))) {
            std::fprintf(ib_qp, "%d,%llu,%u,%u,%u,%u\n", r.rank,
                         (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                         s.payload.pd, s.payload.qp_num, s.payload.qp_type);
        }
        for (const auto& s : load_stream<ibr::IbModifyQp>(stream_path<ibr::IbModifyQp>(r))) {
            std::fprintf(ib_mod, "%d,%llu,%u,%u,%u,%u,%u\n", r.rank,
                         (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                         s.payload.qp_num, s.payload.dest_qp_num, s.payload.dest_lid,
                         s.payload.state);
        }
        for (const auto& s : load_stream<ibr::IbPostSend>(stream_path<ibr::IbPostSend>(r))) {
            std::fprintf(ib_send, "%d,%llu,%u,%llu,0x%llx,%u,%u,%u,%u,%u\n", r.rank,
                         (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                         (unsigned long long)s.payload.wr_id,
                         (unsigned long long)s.payload.remote_addr, s.payload.rkey,
                         s.payload.length, s.payload.qp_num, s.payload.opcode,
                         s.payload.num_sge);
            n_send++;
        }
        for (const auto& s : load_stream<ibr::IbPollCq>(stream_path<ibr::IbPollCq>(r))) {
            std::fprintf(ib_cq, "%d,%llu,%u,%llu,%u,%u,%u,%u\n", r.rank,
                         (unsigned long long)s.temporal.a.ticks, s.payload.cq,
                         (unsigned long long)s.payload.wr_id, s.payload.byte_len,
                         s.payload.qp_num, s.payload.status, s.payload.opcode);
        }
        for (const auto& s : load_stream<ibr::IbMemRegion>(stream_path<ibr::IbMemRegion>(r))) {
            std::fprintf(ib_mr, "%d,%llu,%u,%u,0x%llx,%llu,%u,%u,%u\n", r.rank,
                         (unsigned long long)s.temporal.a.ticks, s.payload.mr,
                         s.payload.pd, (unsigned long long)s.payload.range.base,
                         (unsigned long long)s.payload.range.size, s.payload.lkey,
                         s.payload.rkey, s.payload.access);
        }
    }
    for (std::FILE* f : {access, object, source, anchor,
                         ib_qp, ib_mod, ib_send, ib_cq, ib_mr}) std::fclose(f);

    std::printf("pcomm-read: %zu ranks, %zu gpu-accesses, %zu ib-sends -> %s/"
                "{gpu_access,mem_object,source_line,clock_anchor,ib_*}.csv\n",
                ranks.size(), n_access, n_send, outdir.c_str());
    return 0;
}

// --- Perfetto / Chrome JSON trace --------------------------------------------
// One process per rank, one thread per QP; post_send/poll_cq become instant
// events. TSC ticks aren't comparable across nodes (per-CPU, boot-relative), so
// each rank's ClockAnchors (start+end: local_ticks + wall_ns) calibrate
// ticks -> wall-ns; a global wall-ns origin keeps µs precision and aligns all
// ranks on one axis. Load the output in https://ui.perfetto.dev.
struct RankClock {
    long double t0{0}, w0{0};   // anchor tick / wall_ns
    double ns_per_tick{1.0};
    std::uint64_t wall_at(std::uint64_t tick) const {
        return static_cast<std::uint64_t>(w0 + (static_cast<long double>(tick) - t0) * ns_per_tick);
    }
};

int export_perfetto(const fs::path& root, const fs::path& outfile) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm-read: no rank-* directories under %s\n", root.c_str());
        return 1;
    }
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
        auto dur = [&](const core::Temporal& t) {
            return (t.kind == core::TimeKind::Interval && t.b.ticks > t.a.ticks)
                       ? (t.b.ticks - t.a.ticks) * c.ns_per_tick / 1000.0
                       : 0.0;  // µs; 0 -> instant
        };
        for (const auto& s : load_stream<ibr::IbPostSend>(stream_path<ibr::IbPostSend>(r))) {
            sep();
            double ts = us(c, s.temporal.a.ticks), d = dur(s.temporal);
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
            double ts = us(c, s.temporal.a.ticks), d = dur(s.temporal);
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

// --- raw single-stream dump ---------------------------------------------------

void dump(core::RecordTypeId type, const fs::path& file) {
    switch (type) {
        case core::RecordTrait<GpuAccess>::type_id: {
            std::puts("ticks,pc,object,addr,size,op,run_dev,mem_dev,thread");
            for (const auto& r : load_stream<GpuAccess>(file)) {
                std::printf("%llu,%u,%u,0x%llx,%u,%s,%u,%u,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.pc,
                            r.payload.object, (unsigned long long)r.payload.address,
                            r.payload.size, memop_name(r.payload.op),
                            r.payload.run_device, r.payload.mem_device,
                            r.payload.thread);
            }
            break;
        }
        case core::RecordTrait<MemObject>::type_id: {
            std::puts("ticks,id,name,base,size,device");
            for (const auto& r : load_stream<MemObject>(file)) {
                std::printf("%llu,%u,%u,0x%llx,%llu,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.id,
                            r.payload.name, (unsigned long long)r.payload.range.base,
                            (unsigned long long)r.payload.range.size, r.payload.device);
            }
            break;
        }
        case core::RecordTrait<SourceLine>::type_id: {
            std::puts("pc,file,line");
            for (const auto& r : load_stream<SourceLine>(file)) {
                std::printf("%u,%u,%u\n", r.payload.pc, r.payload.file,
                            r.payload.line);
            }
            break;
        }
        case core::RecordTrait<ClockAnchor>::type_id: {
            std::puts("domain,local_ticks,wall_ns,mono_ns");
            for (const auto& r : load_stream<ClockAnchor>(file)) {
                std::printf("%u,%llu,%llu,%llu\n", r.payload.domain,
                            (unsigned long long)r.payload.local_ticks,
                            (unsigned long long)r.payload.wall_ns,
                            (unsigned long long)r.payload.mono_ns);
            }
            break;
        }
        case core::RecordTrait<ibr::IbMemRegion>::type_id: {
            std::puts("ticks,mr,pd,base,size,lkey,rkey,access");
            for (const auto& r : load_stream<ibr::IbMemRegion>(file)) {
                std::printf("%llu,%u,%u,0x%llx,%llu,0x%x,0x%x,0x%x\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.mr,
                            r.payload.pd, (unsigned long long)r.payload.range.base,
                            (unsigned long long)r.payload.range.size, r.payload.lkey,
                            r.payload.rkey, r.payload.access);
            }
            break;
        }
        case core::RecordTrait<ibr::IbQueuePair>::type_id: {
            std::puts("ticks,qp,pd,qp_num,qp_type");
            for (const auto& r : load_stream<ibr::IbQueuePair>(file)) {
                std::printf("%llu,%u,%u,0x%x,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.qp,
                            r.payload.pd, r.payload.qp_num, r.payload.qp_type);
            }
            break;
        }
        case core::RecordTrait<ibr::IbModifyQp>::type_id: {
            std::puts("ticks,qp,qp_num,dest_qp_num,dest_lid,state");
            for (const auto& r : load_stream<ibr::IbModifyQp>(file)) {
                std::printf("%llu,%u,0x%x,0x%x,0x%x,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.qp,
                            r.payload.qp_num, r.payload.dest_qp_num,
                            r.payload.dest_lid, r.payload.state);
            }
            break;
        }
        case core::RecordTrait<ibr::IbPostSend>::type_id: {
            std::puts("ticks,qp,wr_id,remote_addr,rkey,length,qp_num,opcode,num_sge");
            for (const auto& r : load_stream<ibr::IbPostSend>(file)) {
                std::printf("%llu,%u,%llu,0x%llx,0x%x,%u,0x%x,%u,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.qp,
                            (unsigned long long)r.payload.wr_id,
                            (unsigned long long)r.payload.remote_addr, r.payload.rkey,
                            r.payload.length, r.payload.qp_num, r.payload.opcode,
                            r.payload.num_sge);
            }
            break;
        }
        case core::RecordTrait<ibr::IbPollCq>::type_id: {
            std::puts("ticks,cq,wr_id,byte_len,qp_num,status,opcode");
            for (const auto& r : load_stream<ibr::IbPollCq>(file)) {
                std::printf("%llu,%u,%llu,%u,0x%x,%u,%u\n",
                            (unsigned long long)r.temporal.a.ticks, r.payload.cq,
                            (unsigned long long)r.payload.wr_id, r.payload.byte_len,
                            r.payload.qp_num, r.payload.status, r.payload.opcode);
            }
            break;
        }
        default:
            std::fprintf(stderr, "pcomm-read: no decoder for type %u yet\n", type);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(
            "usage: pcomm-read <trace-dir>                    stream inventory\n"
            "       pcomm-read <trace-dir> --csv <outdir>     export all tables (intern-resolved)\n"
            "       pcomm-read <trace-dir> --perfetto <out>   export Perfetto/Chrome JSON trace\n"
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

    std::printf("streams in %s:\n", dir.c_str());
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".pcs") {
            std::printf("  %-40s %10ju bytes\n",
                        fs::relative(entry.path(), dir).c_str(),
                        static_cast<std::uintmax_t>(entry.file_size()));
        }
    }
    return 0;
}
