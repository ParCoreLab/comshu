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
#include <set>
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
#include "mlx5_records.hpp"

namespace fs = std::filesystem;
using namespace pcomm;
using namespace pcomm::core::records;
namespace ibr = pcomm::providers::ibverbs::records;
namespace mxr = pcomm::providers::mlx5dv::records;
using rec::Stamped;

namespace {

// Stream loading (header validation included) lives in
// pcomm/rec/stream_reader.hpp so it can be tested and so the versioning reader
// uses the same path. It used to be a local template here, where nothing could
// reach it.
using rec::load_stream;

const char* kind_name(core::RecordKind k) {
    switch (k) {
        case core::RecordKind::Event:
            return "event";
        case core::RecordKind::Entity:
            return "entity";
        case core::RecordKind::Artifact:
            return "artifact";
        case core::RecordKind::Edge:
            return "edge";
    }
    return "?";
}

const char* memop_name(core::MemOp op) {
    switch (op) {
        case core::MemOp::Load:
            return "load";
        case core::MemOp::Store:
            return "store";
        case core::MemOp::Atomic:
            return "atomic";
        case core::MemOp::Prefetch:
            return "prefetch";
    }
    return "?";
}

// Stable string names for the ibverbs enums, so CSV columns (and the goldens
// that query them) read as physics — "send", "success", "rtr" — instead of
// raw ABI integers, and stay put if the numeric encoding ever changes. Unknown
// values round-trip losslessly as "op<n>" so nothing silently collapses.
std::string wr_opcode_name(unsigned op) {  // ibv_wr_opcode
    switch (op) {
        case 0:
            return "rdma_write";
        case 1:
            return "rdma_write_imm";
        case 2:
            return "send";
        case 3:
            return "send_imm";
        case 4:
            return "rdma_read";
        case 5:
            return "atomic_cmp_swp";
        case 6:
            return "atomic_fetch_add";
    }
    return "op" + std::to_string(op);
}
std::string wc_opcode_name(unsigned op) {  // ibv_wc_opcode
    switch (op) {
        case 0:
            return "send";
        case 1:
            return "rdma_write";
        case 2:
            return "rdma_read";
        case 3:
            return "comp_swap";
        case 4:
            return "fetch_add";
        case 128:
            return "recv";
        case 129:
            return "recv_rdma_imm";
    }
    return "wc" + std::to_string(op);
}
std::string wc_status_name(unsigned s) {  // ibv_wc_status (0 = success)
    return s == 0 ? "success" : "err" + std::to_string(s);
}
std::string qp_state_name(unsigned st) {  // ibv_qp_state
    switch (st) {
        case 0:
            return "reset";
        case 1:
            return "init";
        case 2:
            return "rtr";
        case 3:
            return "rts";
        case 4:
            return "sqd";
        case 5:
            return "sqe";
        case 6:
            return "err";
    }
    return "st" + std::to_string(st);
}
std::string qp_type_name(unsigned t) {  // ibv_qp_type
    switch (t) {
        case 2:
            return "rc";
        case 3:
            return "uc";
        case 4:
            return "ud";
        case 8:
            return "raw_packet";
    }
    return "qpt" + std::to_string(t);
}

// --- the record descriptor table ---------------------------------------------
// One specialization per exportable record type: table name, column header, and
// THE row printer. `--csv` prefixes each row with the rank; `--dump` prints it
// bare; both call the same row(), so a field's format exists in one place.
template<core::Record R>
struct Table;  // no primary definition: an unspecialized use is a missing entry

template<> struct Table<GpuAccess> {
    static constexpr const char* name = "gpu_access";
    static constexpr const char* cols = "ticks,pc,object,addr,size,op,run_dev,mem_dev,thread";
    static void row(std::FILE* f, const Stamped<GpuAccess>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%llx,%u,%s,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.pc, s.payload.object, (unsigned long long)s.payload.address,
                     s.payload.size, memop_name(s.payload.op), s.payload.run_device,
                     s.payload.mem_device, s.payload.thread);
    }
};

template<> struct Table<MemObject> {
    static constexpr const char* name = "mem_object";
    static constexpr const char* cols = "ticks,id,name,base,size,device";
    static void row(std::FILE* f, const Stamped<MemObject>& s, const rec::InternDictionary& in) {
        std::fprintf(f, "%llu,%u,%s,0x%llx,%llu,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.id, in.str(s.payload.name), (unsigned long long)s.payload.range.base,
                     (unsigned long long)s.payload.range.size, s.payload.device);
    }
};

template<> struct Table<EntityClose> {
    static constexpr const char* name = "entity_close";
    static constexpr const char* cols = "ticks,subject";
    static void row(std::FILE* f, const Stamped<EntityClose>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u", (unsigned long long)s.temporal.a.ticks, s.payload.subject);
    }
};

template<> struct Table<SourceLine> {
    static constexpr const char* name = "source_line";
    static constexpr const char* cols = "pc,file,line";
    static void row(std::FILE* f, const Stamped<SourceLine>& s, const rec::InternDictionary& in) {
        std::fprintf(f, "%u,%s,%u", s.payload.pc, in.str(s.payload.file), s.payload.line);
    }
};

template<> struct Table<ClockAnchor> {
    static constexpr const char* name = "clock_anchor";
    static constexpr const char* cols = "domain,local_ticks,wall_ns,mono_ns";
    static void row(std::FILE* f, const Stamped<ClockAnchor>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%u,%llu,%llu,%llu", s.payload.domain,
                     (unsigned long long)s.payload.local_ticks,
                     (unsigned long long)s.payload.wall_ns, (unsigned long long)s.payload.mono_ns);
    }
};

template<> struct Table<Attribution> {
    static constexpr const char* name = "attribution";
    static constexpr const char* cols = "ticks,subject,object,pred";
    static void row(std::FILE* f, const Stamped<Attribution>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.subject,
                     s.payload.object, static_cast<unsigned>(s.payload.pred));
    }
};

template<> struct Table<ibr::IbMemRegion> {
    static constexpr const char* name = "ib_mem_region";
    static constexpr const char* cols = "ticks,mr,pd,base,size,lkey,rkey,access";
    static void                  row(std::FILE* f, const Stamped<ibr::IbMemRegion>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%llx,%llu,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                                      s.payload.mr, s.payload.pd, (unsigned long long)s.payload.range.base,
                                      (unsigned long long)s.payload.range.size, s.payload.lkey, s.payload.rkey,
                                      s.payload.access);
    }
};

// qp/mr/cq/pd are interned pointer handles (stable within a rank); qp_num /
// dest_qp_num / lid are the wire ids used to stitch the cross-rank comm graph.
template<> struct Table<ibr::IbQueuePair> {
    static constexpr const char* name = "ib_queue_pair";
    static constexpr const char* cols = "ticks,qp,pd,qp_num,qp_type";
    static void                  row(std::FILE* f, const Stamped<ibr::IbQueuePair>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%s", (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                                      s.payload.pd, s.payload.qp_num, qp_type_name(s.payload.qp_type).c_str());
    }
};

template<> struct Table<ibr::IbModifyQp> {
    static constexpr const char* name = "ib_modify_qp";
    static constexpr const char* cols = "ticks,qp,qp_num,dest_qp_num,dest_lid,src_lid,state";
    static void row(std::FILE* f, const Stamped<ibr::IbModifyQp>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%u,%s", (unsigned long long)s.temporal.a.ticks,
                     s.payload.qp, s.payload.qp_num, s.payload.dest_qp_num, s.payload.dest_lid,
                     s.payload.src_lid, qp_state_name(s.payload.state).c_str());
    }
};

template<> struct Table<ibr::IbPostSend> {
    static constexpr const char* name = "ib_post_send";
    static constexpr const char* cols =
        "ticks,qp,wr_id,remote_addr,rkey,length,qp_num,opcode,num_sge";
    static void row(std::FILE* f, const Stamped<ibr::IbPostSend>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,0x%llx,%u,%u,%u,%s,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                     (unsigned long long)s.payload.wr_id, (unsigned long long)s.payload.remote_addr,
                     s.payload.rkey, s.payload.length, s.payload.qp_num,
                     wr_opcode_name(s.payload.opcode).c_str(), s.payload.num_sge);
    }
};

template<> struct Table<ibr::IbPollCq> {
    static constexpr const char* name = "ib_poll_cq";
    static constexpr const char* cols = "ticks,cq,wr_id,byte_len,qp_num,status,opcode";
    static void row(std::FILE* f, const Stamped<ibr::IbPollCq>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,%u,%u,%s,%s", (unsigned long long)s.temporal.a.ticks,
                     s.payload.cq, (unsigned long long)s.payload.wr_id, s.payload.byte_len,
                     s.payload.qp_num, wc_status_name(s.payload.status).c_str(),
                     wc_opcode_name(s.payload.opcode).c_str());
    }
};

// --- mlx5dv/DEVX (200-299) --------------------------------------------------
// The tables that make a GPU-initiated run queryable at all. Under IBGDA the
// ib_* tables above are empty, so these are not a supplement to them — they are
// the only rows there are.

// `binding` and `entry` are decoded to names rather than left as integers for
// the same reason the wc_status/opcode tables exist: a raw 0/1 in a CSV is a
// value nobody checks. Both distinctions are load-bearing — ByDmabuf means the
// address column is an offset and NOT a pointer, and the entry point is how
// double-counted registrations become visible instead of inferred.
inline const char* umem_binding_name(std::uint8_t v) {
    return v == 0 ? "by_addr" : v == 1 ? "by_dmabuf" : "?";
}
inline const char* umem_entry_name(std::uint8_t v) {
    return v == 0 ? "reg" : v == 1 ? "reg_ex" : "?";
}

template<> struct Table<mxr::Mlx5Umem> {
    static constexpr const char* name = "mlx5_umem";
    static constexpr const char* cols =
        "ticks,umem,addr,size,umem_id,access,dmabuf_fd,device,binding,entry";
    static void row(std::FILE* f, const Stamped<mxr::Mlx5Umem>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,0x%llx,%llu,%u,0x%x,%d,%u,%s,%s",
                     (unsigned long long)s.temporal.a.ticks, s.payload.umem,
                     (unsigned long long)s.payload.range.base,
                     (unsigned long long)s.payload.range.size, s.payload.umem_id, s.payload.access,
                     s.payload.dmabuf_fd, s.payload.device, umem_binding_name(s.payload.binding),
                     umem_entry_name(s.payload.entry));
    }
};

template<> struct Table<mxr::Mlx5Uar> {
    static constexpr const char* name = "mlx5_uar";
    static constexpr const char* cols = "ticks,uar,reg_addr,base_addr,page_id,flags";
    static void row(std::FILE* f, const Stamped<mxr::Mlx5Uar>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,0x%llx,0x%llx,%u,0x%x", (unsigned long long)s.temporal.a.ticks,
                     s.payload.uar, (unsigned long long)s.payload.reg_addr,
                     (unsigned long long)s.payload.base_addr, s.payload.page_id, s.payload.flags);
    }
};

template<> struct Table<mxr::Mlx5DevxCmd> {
    static constexpr const char* name = "mlx5_devx_cmd";
    static constexpr const char* cols = "ticks,obj,opcode,decoded";
    static void                  row(std::FILE* f, const Stamped<mxr::Mlx5DevxCmd>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,0x%x,%u", (unsigned long long)s.temporal.a.ticks, s.payload.obj,
                                      s.payload.opcode, s.payload.decoded);
    }
};

template<> struct Table<mxr::Mlx5DevxQp> {
    static constexpr const char* name = "mlx5_devx_qp";
    static constexpr const char* cols =
        "ticks,obj,sq_base,qp_num,wq_umem_id,log_sq_size,device,service_type,gpu_driven";
    static void row(std::FILE* f, const Stamped<mxr::Mlx5DevxQp>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,0x%llx,%u,%u,%u,%u,0x%x,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.obj,
                     (unsigned long long)s.payload.sq_base, s.payload.qp_num, s.payload.wq_umem_id,
                     s.payload.log_sq_size, s.payload.device, s.payload.service_type,
                     s.payload.gpu_driven);
    }
};

template<> struct Table<mxr::Mlx5DevxQpConnect> {
    static constexpr const char* name = "mlx5_devx_qp_connect";
    static constexpr const char* cols =
        "ticks,qp_num,dest_qp_num,dest_lid,src_lid,port_num,dest_gid_prefix,dest_gid_iface";
    static void row(std::FILE* f, const Stamped<mxr::Mlx5DevxQpConnect>& s,
                    const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%u,0x%llx,0x%llx", (unsigned long long)s.temporal.a.ticks,
                     s.payload.qp_num, s.payload.dest_qp_num, s.payload.dest_lid, s.payload.src_lid,
                     s.payload.port_num, (unsigned long long)s.payload.dest_gid_prefix,
                     (unsigned long long)s.payload.dest_gid_iface);
    }
};

template<> struct Table<mxr::Mlx5QueuePair> {
    static constexpr const char* name = "mlx5_queue_pair";
    static constexpr const char* cols = "ticks,qp,pd,qp_num,qp_type";
    static void                  row(std::FILE* f, const Stamped<mxr::Mlx5QueuePair>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                                      s.payload.pd, s.payload.qp_num, s.payload.qp_type);
    }
};

// Every record type with a Table<> above, in table-emission order. This is the
// list everything iterates; adding a record type means specializing Table<> and
// naming it here — the compiler refuses one without the other.
using TableRecords =
    std::tuple<GpuAccess, MemObject, EntityClose, SourceLine, ClockAnchor, Attribution,
               ibr::IbQueuePair, ibr::IbModifyQp, ibr::IbPostSend, ibr::IbPollCq, ibr::IbMemRegion,
               mxr::Mlx5Umem, mxr::Mlx5Uar, mxr::Mlx5DevxCmd, mxr::Mlx5DevxQp,
               mxr::Mlx5DevxQpConnect, mxr::Mlx5QueuePair>;

template<typename Tuple, typename F, std::size_t... Is>
void for_each_type_impl(F&& f, std::index_sequence<Is...>) {
    (f.template operator()<std::tuple_element_t<Is, Tuple>>(), ...);
}
template<typename Tuple, typename F> void for_each_type(F&& f) {
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
    for_each_type<TableRecords>([&]<typename R>() { v.push_back(core::RecordTrait<R>::type_id); });
    return v;
}

// One rank's capture: streams + intern dictionaries.
struct RankDir {
    int                   rank{0};
    fs::path              dir;
    rec::InternDictionary intern;  // pcomm/rec/intern_reader.hpp

    const char* str(core::Handle h) const { return intern.str(h); }
    const char* str(rec::InternDomain d, core::Handle h) const { return intern.str(d, h); }
};

// A rank's stream file for record type R.
template<core::Record R> fs::path stream_path(const RankDir& r) {
    return rec::stream_path<R>(r.dir);
}

// Find every rank-* directory under `root`. A single run lays them out flat
// (root/rank-*); a replayed run nests one level per pass (root/<pass>/rank-*),
// e.g. root/ib/rank-0 and root/gpumem/rank-0 for the same logical rank 0. We
// recurse and accept duplicate rank numbers across passes — each RankDir loads
// its own intern table, so pass-local handles never collide, and because each
// provider emits a disjoint record-type range the passes merge by rank number.
void scan_ranks_into(const fs::path& root, std::vector<RankDir>& ranks) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory())
            continue;
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
    // (lid, qp_num) -> rank. Keyed on the PAIR, not on qp_num: qp_num is
    // allocated per HCA, so two ranks on different nodes routinely draw the
    // same one — 199 collisions in one 8-rank NCCL capture (job 44139911). A
    // qp_num-keyed map resolved every colliding peer to whichever rank was
    // scanned last, producing impossible self-edges (0->0, 3->3) and a peer set
    // of {0,2,3,6} for eight ranks. qp_num IS unique within one HCA, so adding
    // the LID makes the key exact rather than merely better.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> qp_owner;
    // (rank, qp_num) -> (dest_lid, dest_qp_num) from the RTR transition.
    std::map<std::pair<int, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>> rtr_dest;
    std::map<std::pair<int, std::uint32_t>, std::pair<long, long>>
                send_by_qp;  // (rank,qp_num)->(sends,bytes)
    std::size_t n_access = 0, n_send = 0;

    // --- memory attribution -------------------------------------------------
    //
    // Merged by (rank, base, size), which is the buffer's identity. NOT by
    // MemObject.id, and that deviates from the plan for a measured reason:
    // ibverbs and mlx5dv intern the id on the base address in
    // InternDomain::Pointer, but gpumem mints its own dense counter
    // (AllocRegistry::add), and intern handles are dense 1-based counters too.
    // So gpumem's object 3 and an interned pointer handle 3 are the same
    // uint32 meaning two different buffers, and merging on it would fabricate
    // a device disagreement out of an id collision. The range is the fact all
    // three emitters actually agree on.
    struct MemObj {
        std::uint64_t            base{0}, size{0};
        core::DeviceId           lowest_device{0xFFFF};
        std::set<core::DeviceId> devices;  // what the emitters each claimed
        long                     emitters{0};
        long                     refs{0}, ambiguous_refs{0}, nested_refs{0};
    };
    // rank -> (base, size) -> merged object.
    std::map<int, std::map<std::pair<std::uint64_t, std::uint64_t>, MemObj>> objects;
    // rank -> address -> how many times it was referenced. Collected rather
    // than resolved on the fly because the generic export loop reaches
    // gpu_access before mem_object, so containment cannot be answered yet.
    std::map<int, std::unordered_map<std::uint64_t, long>> access_addrs, region_bases;
};

template<core::Record R> void aggregate(CsvAgg&, const RankDir&, const Stamped<R>&) {}

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<GpuAccess>& s) {
    a.n_access++;
    a.access_addrs[r.rank][s.payload.address]++;
}

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<MemObject>& s) {
    auto& o = a.objects[r.rank][{s.payload.range.base, s.payload.range.size}];
    o.base = s.payload.range.base;
    o.size = s.payload.range.size;
    o.devices.insert(s.payload.device);
    o.lowest_device = *o.devices.begin();
    o.emitters++;
}

// A region's base is a LOCAL address, so it can be resolved against this rank's
// objects. ib_post_send deliberately contributes nothing: the only address it
// carries is remote_addr, which lives in the PEER's address space — resolving
// it here would be the qp_num mistake again, in the address domain.
void aggregate(CsvAgg& a, const RankDir& r, const Stamped<ibr::IbMemRegion>& s) {
    a.region_bases[r.rank][s.payload.range.base]++;
}

// NB IbQueuePair deliberately does NOT feed the owner map any more. It is
// emitted at create_qp, and a QP is not bound to a port until the INIT
// transition, so it has no LID to be keyed by — which is exactly why the old
// qp_num-only key collided.

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<ibr::IbModifyQp>& s) {
    // Any transition that knows our LID names this QP. src_lid == 0 means the
    // provider never saw the INIT that binds the port; an unkeyed entry would
    // re-introduce the very collision this replaces, so it is skipped.
    if (s.payload.src_lid)
        a.qp_owner[{s.payload.src_lid, s.payload.qp_num}] = r.rank;
    if (s.payload.state == 2 /*RTR*/ && s.payload.dest_qp_num)
        a.rtr_dest[{r.rank, s.payload.qp_num}] = {s.payload.dest_lid, s.payload.dest_qp_num};
}

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<ibr::IbPostSend>& s) {
    auto& agg = a.send_by_qp[{r.rank, s.payload.qp_num}];
    agg.first += 1;
    agg.second += s.payload.length;
    a.n_send++;
}

// Derived: which memory object owns an address, by range containment, SCOPED TO
// THE RANK.
//
// The rank scope is not decoration. comm_edges learned the same lesson from
// qp_num: it is HCA-local, so two ranks routinely draw the same one, and a
// qp_num-keyed map resolved every colliding peer to whichever rank was scanned
// last. Virtual addresses are worse — every rank's heap starts in the same
// place — so a containment lookup by address alone would attribute one rank's
// buffer to another's access as a matter of course.
//
// One row per (rank, source, object) that something actually referenced, plus a
// row with base = none for references that resolved to nothing. Aggregated like
// comm_edges rather than emitted per address, because gpu_access carries
// ~200k addresses per rank and a per-address table is not a table anyone reads.
//
// Two things this reports rather than resolves:
//
//   ambiguous_refs — references that landed inside more than one object whose
//   ranges PARTIALLY OVERLAP, i.e. neither contains the other. The count is
//   added to every candidate, so the report names all of them; picking one there
//   would be inventing the answer.
//
//   nested_refs — references resolved to the innermost of a containment chain.
//   Reported rather than silent because an implicit choice is still a choice,
//   but it is not a finding: registering sub-ranges of a buffer is ordinary.
//   These two started as one counter, and the conflation made case 02 red on a
//   correct capture (MN5 44348796) — NCCL's co-terminal sub-ranges are nested,
//   not ambiguous.
//
//   devices — how many DISTINCT device ordinals the emitters claimed for one
//   buffer. Two providers observing the same allocation is expected by design
//   (any provider may hook CUDA itself); two providers DISAGREEING about which
//   GPU owns it is a finding. `device` prints the lowest, and `devices > 1` is
//   the flag that says the value is not to be trusted.
void write_mem_owner(const fs::path& outdir, CsvAgg& agg) {
    // Say it out loud, for every merged object and not only the referenced
    // ones: a silent pick here is the failure this whole plan exists to stop.
    for (const auto& [rank, objs] : agg.objects)
        for (const auto& [_key, o] : objs) {
            if (o.devices.size() < 2)
                continue;
            std::fprintf(stderr,
                         "pcomm-read: rank %d: %ld emitters disagree on the device owning "
                         "0x%llx+%llu:",
                         rank, o.emitters, (unsigned long long)o.base, (unsigned long long)o.size);
            for (auto d : o.devices)
                std::fprintf(stderr, " %u", d);
            std::fputs(" — mem_owner reports the lowest; do not trust it\n", stderr);
        }

    std::FILE* f = std::fopen((outdir / "mem_owner.csv").c_str(), "w");
    std::fputs("rank,source,base,size,device,devices,emitters,refs,ambiguous_refs,nested_refs\n",
               f);

    // (rank, source-name, address -> refs)
    const std::pair<const char*, const std::map<int, std::unordered_map<std::uint64_t, long>>*>
        sources[] = {{"gpu_access", &agg.access_addrs}, {"ib_mem_region", &agg.region_bases}};

    for (const auto& [source, addrs_by_rank] : sources) {
        for (const auto& [rank, addrs] : *addrs_by_rank) {
            auto objs_it = agg.objects.find(rank);
            // Copy the rank's objects: refs/ambiguous_refs are per (source,
            // rank), so the merged rows must not accumulate across sources.
            std::vector<CsvAgg::MemObj> objs;
            if (objs_it != agg.objects.end())
                for (const auto& [_key, o] : objs_it->second)
                    objs.push_back(o);

            long unresolved = 0;
            for (const auto& [addr, refs] : addrs) {
                std::size_t hits = 0, last = 0;
                for (std::size_t i = 0; i < objs.size(); ++i)
                    if (objs[i].base <= addr && addr - objs[i].base < objs[i].size) {
                        ++hits;
                        last = i;
                    }
                if (hits == 0) {
                    unresolved += refs;
                    continue;
                }
                if (hits == 1) {
                    objs[last].refs += refs;
                    continue;
                }

                // More than one object contains this address. Two cases, and
                // conflating them is what made case 02 red on a correct capture.
                //
                // NESTED — the candidates form a containment chain, so the
                // smallest sits entirely inside every other one. There is
                // nothing to invent here: the innermost object IS the answer,
                // the way the innermost enclosing scope names a variable.
                // Measured on MN5 job 44348796, NCCL registers co-terminal
                // sub-ranges of one buffer — 0x2374f70+4448, 0x23753c0+3344,
                // 0x2375810+2240, 0x2375c60+1136, every one of them ending at
                // 0x23760D0. That is ordinary allocator behaviour and a board
                // must not go red for it.
                //
                // PARTIAL OVERLAP — two candidates where neither contains the
                // other. No rule picks a winner without inventing one, so every
                // candidate is charged and the row is flagged. This is the case
                // the shared-vocabulary design actually predicted, and it stays
                // a finding.
                // Sentinel rather than 0: objs[0] need not be a candidate at
                // all, and seeding with it would compare against a range that
                // does not contain this address.
                std::size_t inner = objs.size();
                for (std::size_t i = 0; i < objs.size(); ++i)
                    if (objs[i].base <= addr && addr - objs[i].base < objs[i].size &&
                        (inner == objs.size() || objs[i].size < objs[inner].size))
                        inner = i;

                const auto& in_obj = objs[inner];
                bool        chain = true;
                for (std::size_t i = 0; i < objs.size() && chain; ++i) {
                    if (i == inner || !(objs[i].base <= addr && addr - objs[i].base < objs[i].size))
                        continue;
                    chain = objs[i].base <= in_obj.base &&
                            in_obj.base + in_obj.size <= objs[i].base + objs[i].size;
                }

                if (chain) {
                    objs[inner].refs += refs;
                    objs[inner].nested_refs += refs;
                    continue;
                }
                for (auto& o : objs)
                    if (o.base <= addr && addr - o.base < o.size) {
                        o.refs += refs;
                        o.ambiguous_refs += refs;
                    }
            }

            for (const auto& o : objs) {
                if (o.refs == 0)
                    continue;
                std::fprintf(f, "%d,%s,0x%llx,%llu,%u,%zu,%ld,%ld,%ld,%ld\n", rank, source,
                             (unsigned long long)o.base, (unsigned long long)o.size,
                             o.lowest_device, o.devices.size(), o.emitters, o.refs,
                             o.ambiguous_refs, o.nested_refs);
            }
            if (unresolved)
                std::fprintf(f, "%d,%s,none,0,65535,0,0,%ld,0,0\n", rank, source, unresolved);
        }
    }
    std::fclose(f);
}

int export_csv(const fs::path& root, const fs::path& outdir) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm-read: no rank-* directories under %s\n", root.c_str());
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

    write_mem_owner(outdir, agg);

    for (const auto& [_, f] : files)
        std::fclose(f);

    std::printf("pcomm-read: %zu ranks, %zu gpu-accesses, %zu ib-sends -> %s/"
                "{gpu_access,mem_object,source_line,clock_anchor,comm_edges,mem_owner,ib_*}.csv\n",
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
    long double   t0{0}, w0{0};  // anchor tick / wall_ns
    double        ns_per_tick{1.0};
    std::uint64_t wall_at(std::uint64_t tick) const {
        return static_cast<std::uint64_t>(w0 + (static_cast<long double>(tick) - t0) * ns_per_tick);
    }
    // An Interval's duration in ns; 0 for instants (and for backwards intervals,
    // which a broken capture could contain).
    double dur_ns(const core::Temporal& t) const {
        return (t.kind == core::TimeKind::Interval && t.b.ticks > t.a.ticks)
                   ? (t.b.ticks - t.a.ticks) * ns_per_tick
                   : 0.0;
    }
};

// Fits one RankClock per rank and picks the earliest anchored wall-ns as the
// shared origin (0 if no rank has anchors).
std::pair<std::vector<RankClock>, std::uint64_t> build_clocks(const std::vector<RankDir>& ranks) {
    std::vector<RankClock> clk(ranks.size());
    std::uint64_t          origin = ~0ull;
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        auto anchors = load_stream<ClockAnchor>(stream_path<ClockAnchor>(ranks[i]));
        if (!anchors.empty()) {
            const auto& a = anchors.front().payload;
            clk[i].t0 = a.local_ticks;
            clk[i].w0 = a.wall_ns;
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
    if (origin == ~0ull)
        origin = 0;
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
    if (!f) {
        std::fprintf(stderr, "pcomm-read: cannot write %s\n", outfile.c_str());
        return 1;
    }
    std::fputs("{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n", f);
    bool first = true;
    auto sep = [&] {
        if (!first)
            std::fputs(",\n", f);
        first = false;
    };
    auto us = [&](const RankClock& c, std::uint64_t tick) {
        return static_cast<double>(c.wall_at(tick) - origin) / 1000.0;  // ns -> µs
    };

    std::size_t n_ev = 0;
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        const auto& r = ranks[i];
        const auto& c = clk[i];
        sep();
        std::fprintf(f,
                     "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":%d,"
                     "\"args\":{\"name\":\"rank %d\"}}",
                     r.rank, r.rank);
        std::unordered_map<std::uint32_t, bool> seen;
        for (const auto& s : load_stream<ibr::IbQueuePair>(stream_path<ibr::IbQueuePair>(r))) {
            if (seen.emplace(s.payload.qp_num, true).second) {
                sep();
                std::fprintf(f,
                             "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":%d,\"tid\":%u,"
                             "\"args\":{\"name\":\"QP 0x%x\"}}",
                             r.rank, s.payload.qp_num, s.payload.qp_num);
            }
        }
        for (const auto& s : load_stream<ibr::IbPostSend>(stream_path<ibr::IbPostSend>(r))) {
            sep();
            double ts = us(c, s.temporal.a.ticks), d = c.dur_ns(s.temporal) / 1000.0;
            if (d > 0)
                std::fprintf(
                    f,
                    "{\"ph\":\"X\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f,"
                    "\"name\":\"send %uB\",\"args\":{\"opcode\":%u,\"bytes\":%u,\"wr\":%llu}}",
                    r.rank, s.payload.qp_num, ts, d, s.payload.length, s.payload.opcode,
                    s.payload.length, (unsigned long long)s.payload.wr_id);
            else
                std::fprintf(
                    f,
                    "{\"ph\":\"i\",\"s\":\"t\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,"
                    "\"name\":\"send %uB\",\"args\":{\"opcode\":%u,\"bytes\":%u,\"wr\":%llu}}",
                    r.rank, s.payload.qp_num, ts, s.payload.length, s.payload.opcode,
                    s.payload.length, (unsigned long long)s.payload.wr_id);
            n_ev++;
        }
        for (const auto& s : load_stream<ibr::IbPollCq>(stream_path<ibr::IbPollCq>(r))) {
            sep();
            double ts = us(c, s.temporal.a.ticks), d = c.dur_ns(s.temporal) / 1000.0;
            if (d > 0)
                std::fprintf(
                    f,
                    "{\"ph\":\"X\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f,"
                    "\"name\":\"complete\",\"args\":{\"status\":%u,\"opcode\":%u,\"bytes\":%u}}",
                    r.rank, s.payload.qp_num, ts, d, s.payload.status, s.payload.opcode,
                    s.payload.byte_len);
            else
                std::fprintf(
                    f,
                    "{\"ph\":\"i\",\"s\":\"t\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,"
                    "\"name\":\"complete\",\"args\":{\"status\":%u,\"opcode\":%u,\"bytes\":%u}}",
                    r.rank, s.payload.qp_num, ts, s.payload.status, s.payload.opcode,
                    s.payload.byte_len);
            n_ev++;
        }
    }
    std::fputs("\n]}\n", f);
    std::fclose(f);
    std::printf("pcomm-read: %zu ranks, %zu events -> %s (load in ui.perfetto.dev)\n", ranks.size(),
                n_ev, outfile.c_str());
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
    void        varint(std::uint64_t v) {
        while (v >= 0x80) {
            b.push_back(static_cast<char>(v | 0x80));
            v >>= 7;
        }
        b.push_back(static_cast<char>(v));
    }
    void tag(int field, int wire) { varint((static_cast<std::uint64_t>(field) << 3) | wire); }
    void u64(int field, std::uint64_t v) {
        tag(field, 0);
        varint(v);
    }
    void str(int field, const std::string& s) {
        tag(field, 2);
        varint(s.size());
        b += s;
    }
    void msg(int field, const PB& m) {
        tag(field, 2);
        varint(m.b.size());
        b += m.b;
    }
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
    auto ns = [&](const RankClock& c, std::uint64_t tick) { return c.wall_at(tick) - origin; };

    PB   trace;
    auto track = [&](std::uint64_t uuid, const std::string& name, std::uint64_t parent) {
        PB td;
        td.u64(1, uuid);
        td.str(2, name);
        if (parent)
            td.u64(5, parent);
        PB pkt;
        pkt.msg(60, td);
        pkt.u64(10, SEQ);
        trace.msg(1, pkt);
    };
    auto slice = [&](std::uint64_t track_uuid, std::uint64_t t_ns, std::uint64_t dur_ns,
                     const std::string& name) {
        PB beg;
        beg.u64(9, dur_ns ? 1u : 3u);
        beg.u64(11, track_uuid);
        beg.str(23, name);  // BEGIN|INSTANT
        PB p1;
        p1.u64(8, t_ns);
        p1.msg(11, beg);
        p1.u64(10, SEQ);
        trace.msg(1, p1);
        if (dur_ns) {
            PB end;
            end.u64(9, 2u);
            end.u64(11, track_uuid);  // SLICE_END
            PB p2;
            p2.u64(8, t_ns + dur_ns);
            p2.msg(11, end);
            p2.u64(10, SEQ);
            trace.msg(1, p2);
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
                char nm[24];
                std::snprintf(nm, sizeof nm, "QP 0x%x", s.payload.qp_num);
                track(qp_uuid(r.rank, s.payload.qp_num), nm, rank_uuid(r.rank));
            }
        }
        for (const auto& s : load_stream<ibr::IbPostSend>(stream_path<ibr::IbPostSend>(r))) {
            char nm[24];
            std::snprintf(nm, sizeof nm, "send %uB", s.payload.length);
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
    if (!f) {
        std::fprintf(stderr, "pcomm-read: cannot write %s\n", outfile.c_str());
        return 1;
    }
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
        if (core::RecordTrait<R>::type_id != type || matched)
            return;
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
            "       pcomm-read <trace-dir> --pftrace <out>    export Perfetto native protobuf "
            "trace\n"
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
    std::printf("  %-34s %6s %-9s %7s %9s %16s\n", "file", "type", "kind", "stride", "records",
                "schema");
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pcs")
            continue;

        const auto        rel = fs::relative(entry.path(), dir);
        const auto        bytes = static_cast<std::uintmax_t>(entry.file_size());
        rec::StreamHeader h{};
        std::ifstream     in(entry.path(), std::ios::binary);
        if (bytes < sizeof h || !in.read(reinterpret_cast<char*>(&h), sizeof h) ||
            h.magic != rec::kStreamMagic) {
            std::printf("  %-34s %10ju bytes  (no stream header — pre-format-1 "
                        "capture, re-capture it)\n",
                        rel.c_str(), bytes);
            continue;
        }
        const auto payload = bytes - sizeof h;
        std::printf("  %-34s %6u %-9s %7u %9ju %016llx%s\n", rel.c_str(), h.type_id,
                    kind_name(h.kind), h.stride,
                    h.stride ? static_cast<std::uintmax_t>(payload / h.stride) : 0,
                    (unsigned long long)h.schema_hash,
                    h.schema_hash == rec::kSchemaHash ? "" : " (other schema)");
        if (h.stride && payload % h.stride)
            std::printf("  %-34s   ^ truncated: %ju trailing bytes are not a whole record\n", "",
                        static_cast<std::uintmax_t>(payload % h.stride));
    }
    return 0;
}
