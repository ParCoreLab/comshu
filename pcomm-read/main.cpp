// The reading half of pcomm: `read`, `dump`, `export`, `link`, `sql`, `types`.
// Target pipeline: raw per-type streams + intern sidecars -> TimeAligner ->
// columnar Tables -> exporters (Parquet canonical, Perfetto derived). Heavy
// deps (Arrow) are allowed here — off the hot path.
//
// Current stage: relational CSV export with intern resolution
// (`export --csv`), a raw per-stream decoder (`dump`), Perfetto exporters, and
// a stream inventory (`read`). The CSVs are the queryable "log" output until the Parquet writer
// lands (they load directly into DuckDB/Polars/pandas).
//
// Every record type is described exactly once, by a Table<R> specialization:
// its table name, its columns, and how one row prints. The CSV export, `dump`
// and the known-type inventory are all derived from that list, so a new record
// type is one Table<> entry — and the exports cannot drift apart (they did:
// `dump` used to print raw handles where the CSV resolved them).

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <unistd.h>
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
#include "../pcomm/cli.hpp"

// At file scope, deliberately: raw_columns.hpp opens `namespace pcomm::raw`, and
// including it inside this file's anonymous namespace would declare a *nested*
// pcomm that shadows the real one — which is exactly what happened the first
// time. duckdb.h must precede it; the generated appends are duckdb_append_*.
#if PCOMM_HAVE_DUCKDB
extern "C" {
#include "duckdb.h"
}
#include "duck_mount.hpp"  // raw_columns.hpp + the shared mount policy
#include "link_sql.hpp"
#endif

#include "capture_load.hpp"  // scan_ranks, the raw-stream -> DuckDB loop

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
// THE row printer. `export --csv` prefixes each row with the rank; `dump` prints it
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
    static constexpr const char* cols = "ticks,subject,opener";
    static void row(std::FILE* f, const Stamped<EntityClose>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.subject,
                     s.payload.opener);
    }
};

template<> struct Table<SourceLine> {
    static constexpr const char* name = "source_line";
    static constexpr const char* cols = "pc,file,line";
    static void row(std::FILE* f, const Stamped<SourceLine>& s, const rec::InternDictionary& in) {
        std::fprintf(f, "%u,%s,%u", s.payload.pc, in.str(s.payload.file), s.payload.line);
    }
};

// One row per rank: which machine it ran on. `node_hash = 0` means gethostname
// failed, which is deliberately distinct from the record being absent — the
// reader treats 0 as unknown and falls back to LID grouping rather than merging
// every failed rank onto one node.
template<> struct Table<NodeIdentity> {
    static constexpr const char* name = "node_identity";
    static constexpr const char* cols = "node_hash";
    static void row(std::FILE* f, const Stamped<NodeIdentity>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu", (unsigned long long)s.payload.node_hash);
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

// Names for IbMemRegion.origin. A region's door decides whether a missing
// address is a defect or a fact about the memory: DM lives on the NIC and a
// null MR has no memory at all, so neither can ever have a host address.
static const char* ib_region_origin_name(std::uint8_t raw) {
    switch (static_cast<ibr::IbRegionOrigin>(raw)) {
        case ibr::IbRegionOrigin::RegMr:
            return "reg_mr";
        case ibr::IbRegionOrigin::Dmabuf:
            return "dmabuf";
        case ibr::IbRegionOrigin::DeviceMemory:
            return "device_memory";
        case ibr::IbRegionOrigin::NullMr:
            return "null_mr";
        case ibr::IbRegionOrigin::Unknown:
            break;
    }
    return "unknown";
}

template<> struct Table<ibr::IbMemRegion> {
    static constexpr const char* name = "ib_mem_region";
    static constexpr const char* cols = "ticks,mr,pd,base,size,lkey,rkey,access,dmabuf_fd,origin";
    static void                  row(std::FILE* f, const Stamped<ibr::IbMemRegion>& s,
                                     const rec::InternDictionary&) {
        // origin as a NAME, not the raw enumerator: an assertion that has to
        // spell `origin = 3` encodes a number nobody can check against the
        // header, which is how umem_binding earned its own string column.
        std::fprintf(
            f, "%llu,%u,%u,0x%llx,%llu,%u,%u,%u,%d,%s", (unsigned long long)s.temporal.a.ticks,
            s.payload.mr, s.payload.pd, (unsigned long long)s.payload.range.base,
            (unsigned long long)s.payload.range.size, s.payload.lkey, s.payload.rkey,
            s.payload.access, s.payload.dmabuf_fd, ib_region_origin_name(s.payload.origin));
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

// Names for IbCompletionQueue.origin. A raw 1/2 in a CSV is a value nobody
// checks; these are what let an assertion say "no CQ arrived through the
// extended door" in words.
static const char* ib_cq_origin_name(std::uint8_t raw) {
    switch (static_cast<ibr::IbCqOrigin>(raw)) {
        case ibr::IbCqOrigin::CreateCq:
            return "create_cq";
        case ibr::IbCqOrigin::CreateCqEx:
            return "create_cq_ex";
        case ibr::IbCqOrigin::Mlx5dvCreateCq:
            return "mlx5dv_create_cq";
        case ibr::IbCqOrigin::Unknown:
            break;
    }
    return "unknown";
}

// The entity ib_poll_cq.cq joins to. `channel` is 0 when the CQ carries no
// completion channel, which is the same thing as "this CQ can only be polled".
template<> struct Table<ibr::IbCompletionQueue> {
    static constexpr const char* name = "ib_completion_queue";
    static constexpr const char* cols = "ticks,cq,context,channel,cqe,comp_vector,origin";
    static void                  row(std::FILE* f, const Stamped<ibr::IbCompletionQueue>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%u,%s", (unsigned long long)s.temporal.a.ticks,
                                      s.payload.cq, s.payload.context, s.payload.channel, s.payload.cqe,
                                      s.payload.comp_vector, ib_cq_origin_name(s.payload.origin));
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

// The entity IbPostRecv rows of origin 'srq' join to.
template<> struct Table<ibr::IbSharedRecvQueue> {
    static constexpr const char* name = "ib_shared_recv_queue";
    static constexpr const char* cols = "ticks,srq,pd,max_wr,max_sge,srq_limit";
    static void                  row(std::FILE* f, const Stamped<ibr::IbSharedRecvQueue>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                                      s.payload.srq, s.payload.pd, s.payload.max_wr, s.payload.max_sge,
                                      s.payload.srq_limit);
    }
};

// Names for IbPostRecv.origin, for the same reason IbMemRegion.origin has them:
// an assertion that has to spell `origin = 2` encodes a number nobody can check
// against the header.
static const char* ib_recv_origin_name(std::uint8_t raw) {
    switch (static_cast<ibr::IbRecvOrigin>(raw)) {
        case ibr::IbRecvOrigin::QueuePair:
            return "qp";
        case ibr::IbRecvOrigin::SharedRecvQueue:
            return "srq";
        case ibr::IbRecvOrigin::Unknown:
            break;
    }
    return "unknown";
}

// The receive side. `addr`/`lkey` are the first sge's — what resolves the
// buffer to an ib_mem_region, and what makes wr_id on a recv completion join to
// something.
template<> struct Table<ibr::IbPostRecv> {
    static constexpr const char* name = "ib_post_recv";
    static constexpr const char* cols =
        "ticks,subject,wr_id,addr,lkey,length,qp_num,num_sge,origin";
    static void row(std::FILE* f, const Stamped<ibr::IbPostRecv>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,0x%llx,%u,%u,%u,%u,%s",
                     (unsigned long long)s.temporal.a.ticks, s.payload.subject,
                     (unsigned long long)s.payload.wr_id, (unsigned long long)s.payload.addr,
                     s.payload.lkey, s.payload.length, s.payload.qp_num, s.payload.num_sge,
                     ib_recv_origin_name(s.payload.origin));
    }
};

// The UD destination. Without this table a UD workload contributes nothing to
// comm_edges, however much traffic it moves.
template<> struct Table<ibr::IbAddressHandle> {
    static constexpr const char* name = "ib_address_handle";
    static constexpr const char* cols =
        "ticks,ah,pd,dgid_prefix,dgid_iface,flow_label,dlid,sl,port_num,is_global,sgid_index,"
        "hop_limit,traffic_class";
    static void row(std::FILE* f, const Stamped<ibr::IbAddressHandle>& s,
                    const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%llx,0x%llx,%u,%u,%u,%u,%u,%u,%u,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.ah, s.payload.pd,
                     (unsigned long long)s.payload.dgid_prefix,
                     (unsigned long long)s.payload.dgid_iface, s.payload.flow_label, s.payload.dlid,
                     s.payload.sl, s.payload.port_num, s.payload.is_global, s.payload.sgid_index,
                     s.payload.hop_limit, s.payload.traffic_class);
    }
};

// One row per ibv_context. node_guid is what lets two ranks on different nodes
// be joined by device rather than by LID.
template<> struct Table<ibr::IbDevice> {
    static constexpr const char* name = "ib_device";
    static constexpr const char* cols =
        "ticks,context,fw_ver,node_guid,sys_image_guid,vendor_id,vendor_part_id,hw_ver,"
        "phys_port_cnt";
    static void row(std::FILE* f, const Stamped<ibr::IbDevice>& s,
                    const rec::InternDictionary& in) {
        std::fprintf(f, "%llu,%u,%s,0x%llx,0x%llx,0x%x,%u,%u,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.context,
                     in.str(s.payload.fw_ver), (unsigned long long)s.payload.node_guid,
                     (unsigned long long)s.payload.sys_image_guid, s.payload.vendor_id,
                     s.payload.vendor_part_id, s.payload.hw_ver, s.payload.phys_port_cnt);
    }
};

// The counterpart to ib_poll_cq: this CQ was armed, so the thread that owns it
// is sleeping rather than spinning.
template<> struct Table<ibr::IbReqNotifyCq> {
    static constexpr const char* name = "ib_req_notify_cq";
    static constexpr const char* cols = "ticks,cq,solicited_only";
    static void                  row(std::FILE* f, const Stamped<ibr::IbReqNotifyCq>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.cq,
                                      s.payload.solicited_only);
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

// The exact receive identity, from the hardware rather than from the
// application's wr_id. See providers/ibverbs/ib_records.hpp for why these are
// separate rows and not columns on the two tables above.
template<> struct Table<ibr::IbSrqRecvSlot> {
    static constexpr const char* name = "ib_srq_recv_slot";
    static constexpr const char* cols = "ticks,srq,wr_id,addr,srqn,slot";
    static void row(std::FILE* f, const Stamped<ibr::IbSrqRecvSlot>& s,
                    const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,%llu,%u,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.srq, (unsigned long long)s.payload.wr_id,
                     (unsigned long long)s.payload.addr, s.payload.srqn, s.payload.slot);
    }
};

template<> struct Table<ibr::IbRecvCqe> {
    static constexpr const char* name = "ib_recv_cqe";
    static constexpr const char* cols = "ticks,cq,wr_id,srqn,wqe_counter,qp_num,byte_len";
    static void row(std::FILE* f, const Stamped<ibr::IbRecvCqe>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,%u,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.cq, (unsigned long long)s.payload.wr_id, s.payload.srqn,
                     s.payload.wqe_counter, s.payload.qp_num, s.payload.byte_len);
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

template<> struct Table<mxr::Mlx5Doorbell> {
    static constexpr const char* name = "mlx5_doorbell";
    static constexpr const char* cols = "ticks,uar,qp_num,prod_idx";
    static void                  row(std::FILE* f, const Stamped<mxr::Mlx5Doorbell>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.uar,
                                      s.payload.qp_num, s.payload.prod_idx);
    }
};

// opcode -> name, mirroring wr_opcode_name for ib_post_send: a raw 0x08 in a CSV
// is a value nobody checks. mlx5 WQE opcodes (PRM): RDMA_WRITE 0x08, SEND 0x0a,
// RDMA_READ 0x10, ATOMIC_CS 0x11, ATOMIC_FA 0x12.
inline const char* mlx5_wqe_opcode_name(std::uint8_t op) {
    switch (op) {
        case 0x08:
            return "rdma_write";
        case 0x0a:
            return "send";
        case 0x10:
            return "rdma_read";
        case 0x11:
            return "atomic_cs";
        case 0x12:
            return "atomic_fa";
        case 0x14:
            return "atomic_masked_cs";
        case 0x15:
            return "atomic_masked_fa";
        default:
            return "?";
    }
}

template<> struct Table<mxr::Mlx5DeviceSend> {
    static constexpr const char* name = "mlx5_device_send";
    static constexpr const char* cols =
        "ticks,qp_num,wqe_idx,opcode,opcode_name,laddr,lkey,raddr,rkey,length";
    static void row(std::FILE* f, const Stamped<mxr::Mlx5DeviceSend>& s,
                    const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%02x,%s,0x%llx,0x%x,0x%llx,0x%x,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.qp_num, s.payload.wqe_idx,
                     s.payload.opcode, mlx5_wqe_opcode_name(s.payload.opcode),
                     (unsigned long long)s.payload.laddr, s.payload.lkey,
                     (unsigned long long)s.payload.raddr, s.payload.rkey, s.payload.length);
    }
};

// Every record type with a Table<> above, in table-emission order. This is the
// list everything iterates; adding a record type means specializing Table<> and
// naming it here — the compiler refuses one without the other.
// MemObject and EntityClose lead deliberately. The export loop is rank -> type,
// so a type listed earlier is fully read before a later one for that rank —
// which lets a GpuAccess resolve its object's [open, close) window as it
// arrives, instead of buffering every access's timestamp to resolve afterwards.
// Nothing else depends on this order: the CSV files are written independently,
// known_types() is a set, and --types sorts by id.
using TableRecords =
    std::tuple<MemObject, EntityClose, GpuAccess, SourceLine, NodeIdentity, ClockAnchor,
               Attribution, ibr::IbQueuePair, ibr::IbCompletionQueue, ibr::IbModifyQp,
               ibr::IbPostSend, ibr::IbSharedRecvQueue, ibr::IbPostRecv, ibr::IbAddressHandle,
               ibr::IbDevice, ibr::IbReqNotifyCq, ibr::IbPollCq, ibr::IbMemRegion,
               ibr::IbSrqRecvSlot, ibr::IbRecvCqe, mxr::Mlx5Umem,
               mxr::Mlx5Uar, mxr::Mlx5DevxCmd, mxr::Mlx5DevxQp, mxr::Mlx5DevxQpConnect,
               mxr::Mlx5QueuePair, mxr::Mlx5Doorbell, mxr::Mlx5DeviceSend>;

// Moved to capture_load.hpp, with RankDir and the scan, so pcomm-ui can load a
// capture directly instead of only reading a Parquet export of one.
using capture::for_each_type;

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

// The type_id -> table name map, from the same Table<> list every exporter is
// derived from.
//
// It exists because a capture directory carries no vocabulary whatsoever: it is
// `rank-N/<type_id>.pcs` and nothing else, so the stream inventory can only name
// a type by its number. Anything downstream that wants to *label* those numbers
// — the corpus index, and the UI's derived tables after it — would otherwise
// keep a private copy of this mapping, which is the drift `Table<>` exists to
// prevent. Emitting it from the one list means a new record type appears here
// for free, and a renamed table cannot silently disagree with its label.
//
// Takes no trace directory: this is a property of the build, not of a capture.
// Sorted by type_id so the output is diffable across builds.
void print_types() {
    std::vector<std::tuple<unsigned, const char*, unsigned, std::string>> rows;
    for_each_type<TableRecords>([&]<typename R>() {
        rows.emplace_back(core::RecordTrait<R>::type_id, kind_name(core::RecordTrait<R>::kind),
                          core::RecordTrait<R>::version, Table<R>::name);
    });
    // The two intern streams have no Table<> — they are consumed as the
    // dictionary rather than exported as tables — but they are a third of the
    // files in a capture and an index that cannot name them is an index with
    // holes in it. Named from RecordTrait::name, the wire name, so this is still
    // not a hardcoded string.
    auto intern = [&]<typename R>() {
        rows.emplace_back(core::RecordTrait<R>::type_id, kind_name(core::RecordTrait<R>::kind),
                          core::RecordTrait<R>::version, std::string(core::RecordTrait<R>::name));
    };
    intern.template operator()<InternKey>();
    intern.template operator()<InternString>();

    std::sort(rows.begin(), rows.end());
    std::puts("type_id\tkind\tversion\tname");
    for (const auto& [id, kind, ver, name] : rows)
        std::printf("%u\t%s\t%u\t%s\n", id, kind, ver, name.c_str());
}

using capture::RankDir;
using capture::scan_ranks;
using capture::scan_ranks_into;
using capture::stream_path;

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
    // Merged by (rank, base, size), which is the buffer's identity. Still not
    // by MemObject.id, though as of 2026-08-07 that id IS shared — all three
    // emitters intern the base address in InternDomain::Pointer, gpumem
    // included, so joining on it is now sound where it used to collide (its
    // dense AllocRegistry counter and a 1-based intern handle were the same
    // uint32 naming different buffers).
    //
    // The range stays the merge key anyway: it is the fact every emitter agrees
    // on without agreeing on a convention, so this loop does not break if some
    // future emitter interns something else. Joining on the id is a gain for
    // queries, not a reason to make the merge depend on it.
    struct MemObj {
        std::uint64_t            base{0}, size{0};
        core::Handle             id{core::kNullHandle};  // the interned base
        core::DeviceId           lowest_device{0xFFFF};
        std::set<core::DeviceId> devices;  // what the emitters each claimed
        long                     emitters{0};
        long                     refs{0}, ambiguous_refs{0}, nested_refs{0};
        // Of `refs`, how many arrived already attributed — the producer named
        // the object rather than us inferring it from an address. Kept separate
        // because "we were told" and "we worked it out" are different evidence,
        // and a row that is entirely direct needs none of the ambiguity
        // machinery below to be trusted.
        long direct_refs{0};
    };
    // rank -> (base, size) -> merged object.
    std::map<int, std::map<std::pair<std::uint64_t, std::uint64_t>, MemObj>> objects;
    // rank -> address -> how many times it was referenced. Collected rather
    // than resolved on the fly because the generic export loop reaches
    // gpu_access before mem_object, so containment cannot be answered yet.
    std::map<int, std::unordered_map<std::uint64_t, long>> access_addrs, region_bases;
    // One LIFETIME of one object. A handle is a pointer value, so a freed
    // address gets reused and the same handle names a different buffer later —
    // measured on nvshmem_coexist: 63 of 560 region handles and 28 of 522
    // object handles are reused, one of them six times. Merging those would
    // charge one buffer's accesses to another, which is why a handle alone is
    // not an identity and the window is needed.
    struct Life {
        std::uint64_t  open_ticks{0};
        std::uint64_t  close_ticks{~0ull};  // open-ended until EntityClose says otherwise
        std::uint64_t  base{0}, size{0};
        core::DeviceId device{0xFFFF};
        long           refs{0};
    };
    // rank -> object handle -> its lifetimes, in the order they were opened.
    std::map<int, std::unordered_map<core::Handle, std::vector<Life>>> lives;
    // Accesses whose handle matched no lifetime at all, per rank.
    std::map<int, long> unmatched_object_refs;
    // Accesses naming a handle with SEVERAL lifetimes, which only a trustworthy
    // access timestamp could separate. See aggregate(GpuAccess).
    std::map<int, long> ambiguous_object_refs;
};

template<core::Record R> void aggregate(CsvAgg&, const RankDir&, const Stamped<R>&) {}

// An access that names its object is charged to the LIFETIME of that object it
// falls inside; only the unattributed ones fall through to address containment.
// The producer is the better authority for *which* object — it saw the mapping
// the address cannot express — but not for which incarnation, because it hands
// over a handle and handles repeat. The timestamp settles that.
//
// Resolvable in one pass because MemObject and EntityClose lead TableRecords,
// so every lifetime of this rank is already known by the time its accesses
// arrive.
void aggregate(CsvAgg& a, const RankDir& r, const Stamped<GpuAccess>& s) {
    a.n_access++;
    if (s.payload.object == core::kNullHandle) {
        a.access_addrs[r.rank][s.payload.address]++;
        return;
    }
    auto&      by_handle = a.lives[r.rank];
    const auto it = by_handle.find(s.payload.object);
    if (it == by_handle.end()) {
        // The producer named an object we never saw recorded.
        a.unmatched_object_refs[r.rank]++;
        return;
    }

    // ONE lifetime: there is nothing to disambiguate and the timestamp is not
    // consulted. This is the common case and the one that must not be made to
    // depend on a clock we cannot yet trust.
    if (it->second.size() == 1) {
        it->second.front().refs++;
        return;
    }

    // Several lifetimes share this handle, so which buffer was touched depends
    // on WHEN — and gpu_access cannot answer that today. Its `ticks` is the
    // instant the record was DRAINED on the recv thread, not the instant the
    // access happened: measured on the du04 capture, every access carries a
    // timestamp *later* than the EntityClose of the buffer it touched
    // (accesses 45383843401475670 vs close 45383840583777450), because the
    // channel drains after the kernel and after the application freed.
    //
    // So the window is not applied here. Counting these as ambiguous states the
    // limit instead of hiding it — charging them to the newest lifetime, or to
    // all of them, would produce a number that looks resolved and is not.
    // Fixing it is PLAN-20260803-1021-gpumem-window-timestamps, whose own
    // blocker (the positional heap-owner guess) is the thing the nvshmem_ptr
    // rewrite replaced.
    a.ambiguous_object_refs[r.rank]++;
}

// Each MemObject row opens a lifetime. The merged-by-range map below still
// feeds the containment path for unattributed addresses; this list is what the
// handle path resolves against.
void aggregate_life(CsvAgg& a, const RankDir& r, const Stamped<MemObject>& s) {
    a.lives[r.rank][s.payload.id].push_back({s.temporal.a.ticks, ~0ull, s.payload.range.base,
                                             s.payload.range.size, s.payload.device, 0});
}

// Close the most recent still-open lifetime of this subject. Most recent
// because opens and closes interleave: with reuse there can be several, and the
// one being closed is always the live one.
void aggregate(CsvAgg& a, const RankDir& r, const Stamped<EntityClose>& s) {
    if (s.payload.opener != core::records::MemObject::pcomm_type_id)
        return;  // ib/mlx5 entities have their own accounting
    const auto it = a.lives[r.rank].find(s.payload.subject);
    if (it == a.lives[r.rank].end())
        return;
    for (auto life = it->second.rbegin(); life != it->second.rend(); ++life)
        if (life->close_ticks == ~0ull) {
            life->close_ticks = s.temporal.a.ticks;
            return;
        }
}

void aggregate(CsvAgg& a, const RankDir& r, const Stamped<MemObject>& s) {
    aggregate_life(a, r, s);
    auto& o = a.objects[r.rank][{s.payload.range.base, s.payload.range.size}];
    o.base = s.payload.range.base;
    o.size = s.payload.range.size;
    o.devices.insert(s.payload.device);
    o.lowest_device = *o.devices.begin();
    // All emitters intern the base in the Pointer domain, so they mint the same
    // id; keep the first non-null rather than assuming every row carries one.
    if (o.id == core::kNullHandle)
        o.id = s.payload.id;
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
                         "pcomm: rank %d: %ld emitters disagree on the device owning "
                         "0x%llx+%llu:",
                         rank, o.emitters, (unsigned long long)o.base, (unsigned long long)o.size);
            for (auto d : o.devices)
                std::fprintf(stderr, " %u", d);
            std::fputs(" — mem_owner reports the lowest; do not trust it\n", stderr);
        }

    std::FILE* f = std::fopen((outdir / "mem_owner.csv").c_str(), "w");
    std::fputs("rank,source,base,size,device,devices,emitters,refs,direct_refs,"
               "ambiguous_refs,nested_refs\n",
               f);

    // (rank, source-name, address -> refs)
    const std::pair<const char*, const std::map<int, std::unordered_map<std::uint64_t, long>>*>
        sources[] = {{"gpu_access", &agg.access_addrs}, {"ib_mem_region", &agg.region_bases}};

    for (const auto& [source, addrs_by_rank] : sources) {
        const bool is_access = std::strcmp(source, "gpu_access") == 0;

        // Which ranks to visit. Driving this from the address map alone was
        // wrong once every access became producer-attributed: a rank whose
        // accesses ALL carry an object contributes no addresses, so the map is
        // empty for it and the whole rank was silently skipped — mem_owner came
        // back with nothing but a header over a fully attributed capture.
        std::set<int> ranks_here;
        for (const auto& [rank, _addrs] : *addrs_by_rank)
            ranks_here.insert(rank);
        if (is_access)
            for (const auto& [rank, _objs] : agg.lives)
                ranks_here.insert(rank);

        static const std::unordered_map<std::uint64_t, long> kNoAddrs;
        for (const int rank : ranks_here) {
            const auto  addrs_it = addrs_by_rank->find(rank);
            const auto& addrs = addrs_it != addrs_by_rank->end() ? addrs_it->second : kNoAddrs;
            auto        objs_it = agg.objects.find(rank);
            // Copy the rank's objects: refs/ambiguous_refs are per (source,
            // rank), so the merged rows must not accumulate across sources.
            std::vector<CsvAgg::MemObj> objs;
            if (objs_it != agg.objects.end())
                for (const auto& [_key, o] : objs_it->second)
                    objs.push_back(o);

            long unresolved = 0;

            // Producer-attributed accesses are emitted per LIFETIME, below,
            // rather than folded into the merged-by-range rows: two lifetimes
            // of one handle share a base and a size, so merging them would put
            // one buffer's accesses on another's row and no column would say
            // so. Accesses whose handle matched no window are counted
            // unresolved — an access outside every lifetime of the object it
            // claims is a finding, not something to round into the nearest one.
            if (is_access)
                if (const auto u = agg.unmatched_object_refs.find(rank);
                    u != agg.unmatched_object_refs.end())
                    unresolved += u->second;

            // Reported on its own row rather than folded into `unresolved`: the
            // object IS known, only the incarnation is not, and those are
            // different states of knowledge.
            long ambiguous_lives = 0;
            if (is_access)
                if (const auto amb = agg.ambiguous_object_refs.find(rank);
                    amb != agg.ambiguous_object_refs.end())
                    ambiguous_lives = amb->second;

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

            // One row per lifetime that was actually referenced. `direct_refs`
            // equals `refs` here by construction: every one of them was placed
            // by the producer's handle and this instant's window, never by
            // containment.
            if (is_access) {
                if (const auto lv = agg.lives.find(rank); lv != agg.lives.end())
                    for (const auto& [_id, lifetimes] : lv->second)
                        for (const auto& life : lifetimes) {
                            if (life.refs == 0)
                                continue;
                            std::fprintf(f, "%d,%s,0x%llx,%llu,%u,1,1,%ld,%ld,0,0\n", rank, source,
                                         (unsigned long long)life.base,
                                         (unsigned long long)life.size, life.device, life.refs,
                                         life.refs);
                        }
            }

            for (const auto& o : objs) {
                if (o.refs == 0)
                    continue;
                std::fprintf(f, "%d,%s,0x%llx,%llu,%u,%zu,%ld,%ld,%ld,%ld,%ld\n", rank, source,
                             (unsigned long long)o.base, (unsigned long long)o.size,
                             o.lowest_device, o.devices.size(), o.emitters, o.refs, o.direct_refs,
                             o.ambiguous_refs, o.nested_refs);
            }
            if (unresolved)
                std::fprintf(f, "%d,%s,none,0,65535,0,0,%ld,0,0,0\n", rank, source, unresolved);
            if (ambiguous_lives)
                std::fprintf(f, "%d,%s,reused-handle,0,65535,0,0,%ld,0,%ld,0\n", rank, source,
                             ambiguous_lives, ambiguous_lives);
        }
    }
    std::fclose(f);
}

int export_csv(const fs::path& root, const fs::path& outdir) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm: no rank-* directories under %s\n", root.c_str());
        return 1;
    }
    // Checked, not assumed: create_directories THROWS on a path that exists and
    // is not a directory, so `--csv /dev/null` aborted the process with an
    // uncaught filesystem_error instead of saying which path it could not use.
    // Same class as the --sql crash on a missing directory.
    std::error_code ec;
    fs::create_directories(outdir, ec);
    if (!fs::is_directory(outdir)) {
        std::fprintf(stderr, "pcomm: cannot use %s as an output directory: %s\n",
                     outdir.c_str(), ec ? ec.message().c_str() : "not a directory");
        return 1;
    }

    // Say what we cannot represent, before writing tables that omit it.
    std::size_t unhandled = 0;
    for (const auto& r : ranks)
        for (const auto& d : rec::unhandled_streams(r.dir, known_types())) {
            std::fprintf(stderr, "pcomm: rank %d: %s\n", r.rank, d.message.c_str());
            ++unhandled;
        }
    if (unhandled)
        std::fprintf(stderr,
                     "pcomm: %zu stream(s) not exported — this build is older "
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

    std::printf("pcomm: %zu ranks, %zu gpu-accesses, %zu ib-sends -> %s/"
                "{gpu_access,mem_object,source_line,clock_anchor,comm_edges,mem_owner,ib_*}.csv\n",
                ranks.size(), agg.n_access, agg.n_send, outdir.c_str());
    return 0;
}

// --- Parquet export -----------------------------------------------------------
//
// Parquet is the contract between the reader and everything above it: the UI
// queries it through DuckDB, and Polars/pandas read the same files. Nothing
// above this line is allowed to open a `.pcs` — stream headers, schema-hash
// validation, stride checks, version mapping and intern resolution live here and
// only here.
//
// **Raw records, native types, straight from the decoded structs.** A `.pcs` is
// a packed array of fixed-stride PODs, so load_stream already hands back
// `Stamped<R>` with real C++ types — `s.payload.qp_num` IS a uint32_t. Those
// values go to DuckDB's appender as themselves. Nothing is stringified on the
// way, so nothing has to be inferred back.
//
// An earlier attempt staged through CSV and let DuckDB re-derive the types from
// text. It was wrong in a way worth recording, because the failure was silent:
// `mem_object.base` (a uint64_t) came back as the integer 32825344 while
// `ib_device.node_guid` stayed the string "0x946dae030047bf46" — same hex
// notation, different inferred type, decided by whether the value happened to
// exceed int64. A column's type would then depend on which capture you opened.
// Here `base` is UBIGINT because the struct says uint64_t, on every capture.
//
// What this deliberately does NOT do is decode. No opcode names, no resolved
// intern handles, no `rank` joined in from anywhere but the directory. Those
// mappings are semantic, they belong to the derived-table layer, and SQL is a
// better place for them than a C++ printer. `--csv` keeps the decoded columns
// and its goldens are untouched.
#if PCOMM_HAVE_DUCKDB

using pcomm::duck::exec;
constexpr auto duck_exec = [](duckdb_connection c, const std::string& q, const char* w) {
    return pcomm::duck::exec(c, q, w);
};

int export_parquet(const fs::path& root, const fs::path& outdir) {
    auto ranks = scan_ranks(root);
    if (ranks.empty()) {
        std::fprintf(stderr, "pcomm: no rank-* directories under %s\n", root.c_str());
        return 1;
    }
    std::error_code ec;
    fs::create_directories(outdir, ec);

    duckdb_database   db{};
    duckdb_connection con{};
    if (duckdb_open(nullptr, &db) != DuckDBSuccess || duckdb_connect(db, &con) != DuckDBSuccess) {
        std::fprintf(stderr, "pcomm: cannot open an in-memory DuckDB\n");
        return 1;
    }

    // Each table is materialised in DuckDB before COPY, so the peak is the
    // largest table, not the largest rank — measured at 627 MB for a 2.1M-row
    // capture. An in-memory database will not spill without somewhere to spill
    // to, so give it one: a capture an order of magnitude larger should get
    // slower, not killed. (Reading one rank at a time above is a real but much
    // smaller saving; it is not what dominates.)
    duck_exec(con, "SET temp_directory='" + outdir.string() + "'", "temp_directory");

    // The load is capture_load.hpp's, shared with pcomm-ui so that "read a
    // capture into DuckDB" has one implementation. What stays here is the only
    // part that is actually about Parquet: writing each loaded table out.
    auto st = capture::load_raw_tables(con, ranks);

    std::size_t tables = 0, rows_total = st.rows, failed = st.failed;
    for_each_type<raw::RawRecords>([&]<typename R>() {
        using Raw = raw::RawTable<R>;
        // Only tables the load actually created: probing is how we keep the
        // absent/empty distinction the loader established, without repeating
        // its existence rules here and risking the two drifting apart.
        duckdb_result probe{};
        const bool    present =
            duckdb_query(con, ("SELECT 1 FROM \"" + std::string(Raw::table) + "\" LIMIT 0").c_str(),
                         &probe) == DuckDBSuccess;
        duckdb_destroy_result(&probe);
        if (!present)
            return;

        const auto out = (outdir / (std::string(Raw::table) + ".parquet")).string();
        if (duck_exec(con,
                      "COPY \"" + std::string(Raw::table) + "\" TO '" + out + "' (FORMAT PARQUET)",
                      Raw::table))
            tables++;
        else
            failed++;
    });

    duckdb_disconnect(&con);
    duckdb_close(&db);

    std::printf("pcomm: %zu ranks, %zu tables, %zu rows -> %s/*.parquet%s\n", ranks.size(),
                tables, rows_total, outdir.c_str(), failed ? " (SOME FAILED, see above)" : "");
    return failed ? 1 : 0;
}
using pcomm::duck::mount_raw_tables;

// Derive the link tables from an export. Reads Parquet, writes Parquet beside
// it; the raw tables are untouched.
int run_link(const fs::path& dir) {
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm: not a directory: %s\n", dir.c_str());
        return 1;
    }
    duckdb_database   db{};
    duckdb_connection con{};
    if (duckdb_open(nullptr, &db) != DuckDBSuccess || duckdb_connect(db, &con) != DuckDBSuccess) {
        std::fprintf(stderr, "pcomm: cannot open an in-memory DuckDB\n");
        return 1;
    }
    duck_exec(con, "SET temp_directory='" + dir.string() + "'", "temp_directory");

    const auto present = mount_raw_tables(con, dir);
    if (!present) {
        std::fprintf(stderr, "pcomm: no raw .parquet tables in %s — run `pcomm export --parquet` first\n",
                     dir.c_str());
        duckdb_disconnect(&con);
        duckdb_close(&db);
        return 1;
    }

    std::size_t failed = 0;
    for (const auto& v : link::kViews)
        if (!duck_exec(con, v.sql, v.name))
            failed++;

    std::size_t written = 0;
    for (const auto& t : link::kTables) {
        const auto out = (dir / (std::string(t.name) + ".parquet")).string();
        if (duck_exec(con, "COPY (" + std::string(t.sql) + ") TO '" + out + "' (FORMAT PARQUET)",
                      t.name))
            written++;
        else
            failed++;
    }

    // The health table is derived like the others but reported here, because a
    // number nobody reads is a number nobody acts on. Every one of these should
    // be zero; a non-zero row is a defect in the link, not in the workload.
    duckdb_result res;
    if (duckdb_query(con, "SELECT finding, n FROM link_health WHERE n > 0", &res) ==
        DuckDBSuccess) {
        for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
            char* f = duckdb_value_varchar(&res, 0, row);
            char* n = duckdb_value_varchar(&res, 1, row);
            std::fprintf(stderr, "pcomm: link_health: %s = %s\n", f ? f : "?", n ? n : "?");
            duckdb_free(f);
            duckdb_free(n);
        }
    }
    duckdb_destroy_result(&res);

    duckdb_disconnect(&con);
    duckdb_close(&db);
    std::printf("pcomm: %zu raw tables -> %zu derived -> %s/*.parquet%s\n", present, written,
                dir.c_str(), failed ? " (SOME FAILED, see above)" : "");
    return failed ? 1 : 0;
}

// Query an exported directory. Takes the EXPORT, not the capture: re-reading
// every `.pcs` per query would make an interactive session unusable on a capture
// of any size, and the export is the contract everything above the reader is
// supposed to use anyway. Verifying that contract is the immediate reason this
// exists — "the Parquet reproduces the CSV" is not a claim anyone can check
// without a way to query the Parquet.
//
// Results are printed by DuckDB itself, via COPY to stdout, rather than walked
// with the C result API. That is not a shortcut: the row-wise accessors are
// deprecated, and the supported alternative means hand-decoding every column
// type a query might return. COPY handles all of them and cannot drift.
int run_sql(const fs::path& dir, const std::string& query) {
    // Checked rather than assumed: directory_iterator *throws* on a missing
    // path, so a mistyped directory used to abort with an uncaught
    // filesystem_error instead of saying which directory it could not find.
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm: not a directory: %s\n", dir.c_str());
        return 1;
    }
    duckdb_database   db{};
    duckdb_connection con{};
    if (duckdb_open(nullptr, &db) != DuckDBSuccess || duckdb_connect(db, &con) != DuckDBSuccess) {
        std::fprintf(stderr, "pcomm: cannot open an in-memory DuckDB\n");
        return 1;
    }

    // Parquet preferred, CSV accepted. A --csv export is a perfectly good thing
    // to query, and refusing it would mean the suite's existing outputs are
    // unreachable from here for no reason.
    std::size_t views = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension();
        const bool parquet = ext == ".parquet";
        if (!parquet && ext != ".csv")
            continue;
        const auto name = entry.path().stem().string();
        const auto reader = parquet ? "read_parquet('" : "read_csv('";
        const auto suffix = parquet ? "')" : "', header=true, auto_detect=true, sample_size=-1)";
        if (duck_exec(con,
                      "CREATE OR REPLACE VIEW \"" + name + "\" AS SELECT * FROM " + reader +
                          entry.path().string() + suffix,
                      name.c_str()))
            views++;
    }
    if (!views) {
        std::fprintf(stderr, "pcomm: no .parquet or .csv tables in %s\n", dir.c_str());
        duckdb_disconnect(&con);
        duckdb_close(&db);
        return 1;
    }

    // Via a temp file, NOT `COPY ... TO '/dev/stdout'`. That spelling looks
    // tidier and is wrong: when stdout is a redirected file DuckDB opens it and
    // seeks to offset 0, silently overwriting whatever the process already
    // wrote. Two `sql` runs into one redirect lost the first, and so would any
    // `pcomm sql ... > out.csv` that printed anything beforehand.
    const auto tmp =
        fs::temp_directory_path() / ("pcomm-sql-" + std::to_string(::getpid()) + ".csv");
    const bool ok = duck_exec(
        con, "COPY (" + query + ") TO '" + tmp.string() + "' (FORMAT CSV, HEADER)", "query");
    if (ok) {
        if (std::FILE* f = std::fopen(tmp.c_str(), "rb")) {
            char        buf[64 * 1024];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                std::fwrite(buf, 1, n, stdout);
            std::fclose(f);
        }
    }
    std::error_code ec;
    fs::remove(tmp, ec);

    duckdb_disconnect(&con);
    duckdb_close(&db);
    return ok ? 0 : 1;
}
#endif  // PCOMM_HAVE_DUCKDB

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
        std::fprintf(stderr, "pcomm: no rank-* directories under %s\n", root.c_str());
        return 1;
    }
    auto [clk, origin] = build_clocks(ranks);

    std::FILE* f = std::fopen(outfile.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "pcomm: cannot write %s\n", outfile.c_str());
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
    std::printf("pcomm: %zu ranks, %zu events -> %s (load in ui.perfetto.dev)\n", ranks.size(),
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
        std::fprintf(stderr, "pcomm: no rank-* directories under %s\n", root.c_str());
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
        std::fprintf(stderr, "pcomm: cannot write %s\n", outfile.c_str());
        return 1;
    }
    std::fwrite(trace.b.data(), 1, trace.b.size(), f);
    std::fclose(f);
    std::printf("pcomm: %zu ranks, %zu events -> %s (%zu bytes; load in ui.perfetto.dev)\n",
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
        std::fprintf(stderr, "pcomm read: no decoder for type %u yet\n", type);
}

// A DuckDB-only verb in a build without it. Says which build this is and how to
// get the other one — a bare "unknown option" would read as a version problem
// rather than a build one.
#if !PCOMM_HAVE_DUCKDB
int no_duckdb(const char* verb) {
    std::fprintf(stderr,
                 "pcomm %s: this build has no DuckDB.\n"
                 "  tools/get_duckdb.sh && cmake -B build -DPCOMM_DUCKDB=ON\n"
                 "  (needs network; MN5 has none — `pcomm export --csv` is the path there)\n",
                 verb);
    return 1;
}
#endif

void read_usage() {
    std::fputs("usage: pcomm read   <capture>                     stream inventory\n"
               "       pcomm dump   <capture> <type-id>           decode one raw stream\n"
               "       pcomm export <capture> --csv      <outdir> all tables, intern-resolved\n"
               "       pcomm export <capture> --parquet  <outdir> all tables as Parquet\n"
               "       pcomm export <capture> --perfetto <out>    Perfetto/Chrome JSON trace\n"
               "       pcomm export <capture> --pftrace  <out>    Perfetto native protobuf\n"
               "       pcomm link   <export>                      derive the rank-space tables\n"
               "       pcomm sql    <export> \"<query>\"            query an exported directory\n"
               "       pcomm types                                type-id -> table name\n"
               "\n"
               "  A <capture> holds rank-* directories of .pcs streams; an <export> is what\n"
               "  `pcomm export --csv/--parquet` wrote. They are different things, which is\n"
               "  why they are different verbs.\n",
               stderr);
}

}  // namespace

int pcomm::cli::read_main(int argc, char** argv) {
    const std::string_view verb = argc >= 1 ? argv[0] : "read";

    // Describes the build, not a capture, so it is answered before anything
    // looks at a directory.
    if (verb == "types") {
        print_types();
        return 0;
    }

    if (verb == "link") {
        if (argc < 2) {
            std::fputs("usage: pcomm link <export>\n", stderr);
            return 1;
        }
#if PCOMM_HAVE_DUCKDB
        return run_link(argv[1]);
#else
        return no_duckdb("link");
#endif
    }

    if (verb == "sql") {
        if (argc < 3) {
            std::fputs("usage: pcomm sql <export> \"<query>\"\n", stderr);
            return 1;
        }
#if PCOMM_HAVE_DUCKDB
        return run_sql(argv[1], argv[2]);
#else
        return no_duckdb("sql");
#endif
    }

    // Everything below takes a capture directory as its first argument.
    if (argc < 2) {
        read_usage();
        return 1;
    }
    fs::path dir = argv[1];
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm %.*s: not a directory: %s\n", (int)verb.size(), verb.data(),
                     dir.c_str());
        return 1;
    }

    if (verb == "export") {
        if (argc < 4) {
            std::fputs("usage: pcomm export <capture> --csv|--parquet|--perfetto|--pftrace <out>\n",
                       stderr);
            return 1;
        }
        const std::string_view fmt = argv[2];
        if (fmt == "--csv")
            return export_csv(dir, argv[3]);
        if (fmt == "--perfetto")
            return export_perfetto(dir, argv[3]);
        if (fmt == "--pftrace")
            return export_pftrace(dir, argv[3]);
        if (fmt == "--parquet") {
#if PCOMM_HAVE_DUCKDB
            return export_parquet(dir, argv[3]);
#else
            return no_duckdb("export --parquet");
#endif
        }
        std::fprintf(stderr, "pcomm export: unknown format '%s'\n", argv[2]);
        return 1;
    }

    if (verb == "dump") {
        if (argc < 3) {
            std::fputs("usage: pcomm dump <capture> <type-id>   (ids: pcomm types)\n", stderr);
            return 1;
        }
        auto type = static_cast<core::RecordTypeId>(std::atoi(argv[2]));
        bool found = false;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() &&
                entry.path().filename() == std::to_string(type) + ".pcs") {
                dump(type, entry.path());
                found = true;
            }
        }
        if (!found) {
            std::fprintf(stderr, "pcomm dump: no %u.pcs under %s\n", type, dir.c_str());
            return 1;
        }
        return 0;
    }

    // `read`: the inventory reads each file's own header rather than inferring
    // anything from its name or its size. This is the first thing in the tree
    // that is told what a capture is by the capture itself.
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
