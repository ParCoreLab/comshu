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
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
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
#include "nccl_records.hpp"
#include "ucx_deep_records.hpp"
#include "ucx_records.hpp"
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
namespace ucr = pcomm::providers::ucx::records;
namespace udr = pcomm::providers::ucx_deep::records;
namespace ncr = pcomm::providers::nccl::records;
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

// ticks_lo/ticks_hi, not `ticks`: a GPU access is a Window, not an Instant. The
// device has no clock we share, so what is known is that the access happened
// after its kernel was launched and before the host drained it from the NVBit
// channel — see RawAccess in pcomm/attr/interfaces.hpp. Two named columns
// because one called `ticks` was printed identically to ib_post_send's `ticks`,
// which is a *measured* call-entry time; joining the two silently correlated
// drain time against wire time. ticks_lo == 0 means the lower bound is open.
//
// `launch` is GpuKernel.correlation on the same rank, and it is what makes that
// wide window narrowable — the derived layer joins it to the kernel's own
// %globaltimer span, which is three orders tighter. It is a key rather than a
// tighter timestamp for an ordering reason spelled out on GpuAccess::launch.
template<> struct Table<GpuAccess> {
    static constexpr const char* name = "gpu_access";
    static constexpr const char* cols =
        "ticks_lo,ticks_hi,launch,pc,object,addr,size,op,run_dev,mem_dev,thread";
    static void row(std::FILE* f, const Stamped<GpuAccess>& s, const rec::InternDictionary&) {
        std::fprintf(
            f, "%llu,%llu,%llu,%u,%u,0x%llx,%u,%s,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
            (unsigned long long)s.temporal.b.ticks, (unsigned long long)s.payload.launch,
            s.payload.pc, s.payload.object, (unsigned long long)s.payload.address, s.payload.size,
            memop_name(s.payload.op), s.payload.run_device, s.payload.mem_device, s.payload.thread);
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

// What a device ordinal actually refers to. See core/records.hpp GpuDevice for
// why an ordinal is not an identity and why the key is (node_hash, pci_bus).
//
// pci_bus is printed DECODED, as domain:bus:device, because that is the form
// `nvidia-smi --query-gpu=pci.bus_id` prints and the only reason a reader wants
// this column is to line the two up. The packed uint32 is an internal
// convenience and putting it on a human-facing surface would make every
// comparison a mental shift-and-mask.
//
// The UUID is printed as the 32 hex digits `nvidia-smi` shows after "GPU-",
// without the dashes: two 64-bit halves, high first, which is the byte order
// cuDeviceGetUuid fills. 0,0 means unavailable and prints as zeroes — the
// schema's documented sentinel, and NOT a real all-zero UUID, which is a
// distinction the reader has to keep because a degenerate UUID is exactly the
// hazard this record was designed around.
template<> struct Table<GpuDevice> {
    static constexpr const char* name = "gpu_device";
    static constexpr const char* cols = "ordinal,pci_bus,uuid,name";
    static void row(std::FILE* f, const Stamped<GpuDevice>& s, const rec::InternDictionary& in) {
        std::fprintf(f, "%u,%04x:%02x:%02x,%016llx%016llx,%s", s.payload.ordinal,
                     (s.payload.pci_bus >> 16) & 0xFFFFu, (s.payload.pci_bus >> 8) & 0xFFu,
                     s.payload.pci_bus & 0xFFu, (unsigned long long)s.payload.uuid_hi,
                     (unsigned long long)s.payload.uuid_lo,
                     in.str(rec::InternDomain::KernelName, s.payload.name));
    }
};

// What this rank was asked to do, and by which build. See core/records.hpp
// Invocation.
//
// THE FIRST RECORD WHOSE PAYLOAD IS FREE TEXT, and therefore the first that
// needs real CSV quoting. Every other interned string in this format is an
// identifier — a kernel name, a transport name, a hex handle — and got away
// without it. These four fields contain commas (argv), '=' and newlines (env),
// and spaces and newlines (images), so an unquoted row would silently produce
// extra columns and extra rows in anything that read it back.
//
// RFC 4180: wrap in quotes, double any embedded quote. DuckDB's read_csv
// understands both, including the embedded newlines, which is what keeps
// `pcomm sql` on a --csv export working on this table.
static std::string csv_quoted(const char* s) {
    std::string out(1, '"');
    for (const char* p = s ? s : ""; *p; ++p) {
        if (*p == '"')
            out += "\"\"";
        else
            out += *p;
    }
    out += '"';
    return out;
}

template<> struct Table<Invocation> {
    static constexpr const char* name = "invocation";
    static constexpr const char* cols = "pid,argv,providers,build,images,env";
    static void row(std::FILE* f, const Stamped<Invocation>& s, const rec::InternDictionary& in) {
        auto t = [&in](core::Handle h) { return csv_quoted(h ? in.str(h) : ""); };
        std::fprintf(f, "%u,%s,%s,%s,%s,%s", s.payload.pid, t(s.payload.argv).c_str(),
                     t(s.payload.providers).c_str(), t(s.payload.build).c_str(),
                     t(s.payload.images).c_str(), t(s.payload.env).c_str());
    }
};

// One process's mapping of a fabric-shared allocation.
//
// `allocation` is emitted as the interned STRING, not its handle: it is the
// join key between two ranks' rows, and a reader that had to resolve handles
// through the intern dictionary to compare them would make the one query this
// record exists for the awkward one. The hex is 128 chars and there is one row
// per shared allocation, so the width costs nothing.
template<> struct Table<PeerMapping> {
    static constexpr const char* name = "peer_mapping";
    static constexpr const char* cols = "allocation,base,size,alloc_offset,device,exporter";
    static void row(std::FILE* f, const Stamped<PeerMapping>& s, const rec::InternDictionary& d) {
        const char* a = d.str(s.payload.allocation);
        std::fprintf(
            f, "%s,%llu,%llu,%llu,%u,%u", a ? a : "", (unsigned long long)s.payload.range.base,
            (unsigned long long)s.payload.range.size, (unsigned long long)s.payload.alloc_offset,
            (unsigned)s.payload.device, (unsigned)s.payload.exporter);
    }
};

// One row per kernel execution. `source` is carried rather than dropped
// because it changes what the interval MEANS: HostSubmit rows are submission,
// not execution, and trail the real thing by milliseconds — a consumer that
// cannot tell them apart will draw them as lifetimes. Ticks are the [a, b]
// pair, in the domain the source's ClockAnchor calibrates.
template<> struct Table<GpuKernel> {
    static constexpr const char* name = "gpu_kernel";
    static constexpr const char* cols =
        "ticks,end_ticks,domain,name,correlation,stream,device,source,grid_x,grid_y,grid_z,"
        "block_x,block_y,block_z";
    static void row(std::FILE* f, const Stamped<GpuKernel>& s, const rec::InternDictionary& in) {
        // KernelName, explicitly. The single-argument in.str() defaults to
        // InternDomain::String, and handles restart at 1 in EVERY domain — so
        // that overload silently resolved a valid handle 1 to "" here. The
        // dictionary's own header says as much; it is worth not rediscovering.
        std::fprintf(f, "%llu,%llu,%u,%s,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u",
                     (unsigned long long)s.temporal.a.ticks, (unsigned long long)s.temporal.b.ticks,
                     s.temporal.a.domain, in.str(rec::InternDomain::KernelName, s.payload.name),
                     (unsigned long long)s.payload.correlation,
                     (unsigned long long)s.payload.stream, s.payload.device, s.payload.source,
                     s.payload.grid_x, s.payload.grid_y, s.payload.grid_z, s.payload.block_x,
                     s.payload.block_y, s.payload.block_z);
    }
};

// One row per host-initiated memory copy. The only lane in a capture that sees
// copy-engine traffic at all — see core/records.hpp CudaMemcpy for why nothing
// else can.
//
// **THE ENUM COLUMNS ARE NUMBERS HERE, NOT NAMES, AND THAT IS DELIBERATE.**
// `gpu_access.op` prints `Store` while the Parquet raw table (generated from the
// same struct by tools/gen_raw_columns.py) holds `1`, so no single SQL assertion
// can name it — and tests/hpc/suite/sql_check.sh runs assertions against the
// PARQUET export when one exists and falls back to CSV+sqlite when it does not.
// An assertion about copies has to work under both engines, so the two exports
// must agree, and agreeing on the number is the only option: the generator
// cannot emit a name without being told semantics it explicitly refuses to
// claim. The names live in the `vocabulary` derived table, joinable from either.
//
// `src_device`/`dst_device` print 65535 for a host end. Not blank and not 0 —
// 0 is a real GPU, and the sentinel is the record's, not the printer's. They are
// SPELLED EXACTLY AS THE STRUCT FIELDS, which is the second half of the same
// argument: gen_raw_columns.py names Parquet columns after the C++ fields
// mechanically, so any shortening here — `src_dev` was the first spelling —
// makes the CSV and the Parquet disagree about one column's name. That is the
// nccl_comm.rank defect the generator's RENAMES table exists to correct, and it
// is cheaper not to create it. Measured on du04: an assertion written against
// `src_dev` ran clean under sqlite and failed under DuckDB with
// `Referenced column "src_dev" not found`, on the same capture.
template<> struct Table<CudaMemcpy> {
    static constexpr const char* name = "cuda_memcpy";
    static constexpr const char* cols = "ticks,end_ticks,domain,correlation,stream,bytes,"
                                        "kind,src_device,dst_device,ctx_device,"
                                        "src_space,dst_space,source,async";
    static void row(std::FILE* f, const Stamped<CudaMemcpy>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%llu,%u,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u",
                     (unsigned long long)s.temporal.a.ticks, (unsigned long long)s.temporal.b.ticks,
                     s.temporal.a.domain, (unsigned long long)s.payload.correlation,
                     (unsigned long long)s.payload.stream, (unsigned long long)s.payload.bytes,
                     unsigned(static_cast<std::uint8_t>(s.payload.kind)), s.payload.src_device,
                     s.payload.dst_device, s.payload.ctx_device,
                     unsigned(static_cast<std::uint8_t>(s.payload.src_space)),
                     unsigned(static_cast<std::uint8_t>(s.payload.dst_space)),
                     unsigned(static_cast<std::uint8_t>(s.payload.source)), s.payload.async);
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

// One row per rank, and the row a reader should look at FIRST: it is the only
// thing in a capture that says whether the rest of it is complete. A rank that
// appears in clock_anchor and not here did not finish. See core/records.hpp.
template<> struct Table<CaptureStatus> {
    static constexpr const char* name = "capture_status";
    static constexpr const char* cols =
        "records_lost,bytes_lost,streams_open,streams_lost,dir_failed,header_lost,never_opened";
    static void row(std::FILE* f, const Stamped<CaptureStatus>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%llu,%u,%u,%u,%u,%u", (unsigned long long)s.payload.records_lost,
                     (unsigned long long)s.payload.bytes_lost, s.payload.streams_open,
                     s.payload.streams_lost, s.payload.dir_failed, s.payload.header_lost,
                     s.payload.never_opened);
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

// ibv_send_flags, spelled. SIGNALED is the bit the send->completion matching
// depends on; the rest are kept because they change how a send is issued and a
// raw 10 in a CSV is a value nobody can read.
inline std::string send_flags_name(std::uint32_t v) {
    if (!v)
        return "none";
    std::string out;
    auto        bit = [&](std::uint32_t m, const char* n) {
        if (v & m) {
            if (!out.empty())
                out += '|';
            out += n;
        }
    };
    bit(1, "fence");
    bit(2, "signaled");
    bit(4, "solicited");
    bit(8, "inline");
    bit(16, "ip_csum");
    return out.empty() ? "none" : out;
}

// ibv_port_state, spelled. 4 is ACTIVE and is the only one that means traffic
// can flow; a LID on a DOWN port explains an absence rather than reporting one.
// Values from enum ibv_port_state, and they start at ZERO — this table was
// written off by one and the first real capture printed MN5's active ports as
// "armed", which is a plausible-looking lie rather than an obvious one. That is
// the failure mode a name table has: nothing crashes, the wrong word just sits
// there. Checked against infiniband/verbs.h.
inline const char* port_state_name(std::uint8_t v) {
    switch (v) {
        case 0:
            return "nop";
        case 1:
            return "down";
        case 2:
            return "init";
        case 3:
            return "armed";
        case 4:
            return "active";
        case 5:
            return "active_defer";
        default:
            return "unknown";
    }
}

template<> struct Table<ibr::IbPort> {
    static constexpr const char* name = "ib_port";
    static constexpr const char* cols = "ticks,context,port_num,lid,state";
    static void row(std::FILE* f, const Stamped<ibr::IbPort>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%s", (unsigned long long)s.temporal.a.ticks,
                     s.payload.context, s.payload.port_num, s.payload.lid,
                     port_state_name(s.payload.state));
    }
};

// One local buffer of a posted send. `addr` is printed as hex for the reason
// every address in this file is: it is matched against mem_object ranges by
// eye at least as often as by query.
template<> struct Table<ibr::IbPostSendSg> {
    static constexpr const char* name = "ib_post_send_sg";
    static constexpr const char* cols = "ticks,qp,wr_id,idx,addr,length,lkey,qp_num";
    static void                  row(std::FILE* f, const Stamped<ibr::IbPostSendSg>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,%u,0x%llx,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                                      s.payload.qp, (unsigned long long)s.payload.wr_id, s.payload.index,
                                      (unsigned long long)s.payload.addr, s.payload.length, s.payload.lkey,
                                      s.payload.qp_num);
    }
};

// sq_sig_all printed as the claim it makes rather than as a bit. "all" means
// every work request on this queue pair completes, which is what certifies the
// send->completion matching in _wr_flight; "selective" means the caller decided
// per work request and the capture cannot say which.
template<> struct Table<ibr::IbQpSignalling> {
    static constexpr const char* name = "ib_qp_signalling";
    static constexpr const char* cols = "ticks,qp,qp_num,sq_sig_all";
    static void                  row(std::FILE* f, const Stamped<ibr::IbQpSignalling>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%s", (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                                      s.payload.qp_num, s.payload.sq_sig_all ? "all" : "selective");
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
        "ticks,qp,wr_id,remote_addr,rkey,length,qp_num,send_flags,opcode,num_sge";
    static void row(std::FILE* f, const Stamped<ibr::IbPostSend>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,0x%llx,%u,%u,%u,%s,%s,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.qp,
                     (unsigned long long)s.payload.wr_id, (unsigned long long)s.payload.remote_addr,
                     s.payload.rkey, s.payload.length, s.payload.qp_num,
                     send_flags_name(s.payload.send_flags).c_str(),
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
        "ticks,subject,wr_id,addr,lkey,length,first_len,qp_num,slot,num_sge,origin";
    static void row(std::FILE* f, const Stamped<ibr::IbPostRecv>& s, const rec::InternDictionary&) {
        // kSlotUnknown is written as -1 rather than 4294967295: the column is a
        // ring index and a reader that sums or maxes it should get an obviously
        // wrong answer from the sentinel, not a plausible one.
        std::fprintf(f, "%llu,%u,%llu,0x%llx,%u,%u,%u,%u,%lld,%u,%s",
                     (unsigned long long)s.temporal.a.ticks, s.payload.subject,
                     (unsigned long long)s.payload.wr_id, (unsigned long long)s.payload.addr,
                     s.payload.lkey, s.payload.length, s.payload.first_len, s.payload.qp_num,
                     s.payload.slot == ibr::kSlotUnknown ? -1LL : (long long)s.payload.slot,
                     s.payload.num_sge, ib_recv_origin_name(s.payload.origin));
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
template<> struct Table<ibr::IbRecvCqe> {
    static constexpr const char* name = "ib_recv_cqe";
    static constexpr const char* cols = "ticks,cq,wr_id,srq,wqe_counter,qp_num,byte_len";
    static void row(std::FILE* f, const Stamped<ibr::IbRecvCqe>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,%u,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.cq, (unsigned long long)s.payload.wr_id, s.payload.srq,
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

// --- UCX, 600-699 -----------------------------------------------------------
// The public-UCP records say what the application ASKED the transport to do.
// What the wire actually did belongs to the ibverbs and mlx5 tables, and on
// MN5's mlx5 transport the two do NOT meet at ibv_post_send: job 44635003 ran
// both providers together and recorded verbs setup, QP/CQ, memory and
// ib_post_recv streams while ib_post_send stayed empty, because UCX posts its
// send WQEs below the public verbs door. So these tables stand on their own for
// now, and nothing here should be read as a claim about bytes on the wire —
// see plans/PLAN-20260815-1540 for the attribution work that would join them.

template<> struct Table<ucr::UcxContext> {
    static constexpr const char* name = "ucx_context";
    static constexpr const char* cols = "ticks,context,api_major,api_minor";
    static void row(std::FILE* f, const Stamped<ucr::UcxContext>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.context,
                     s.payload.api_major, s.payload.api_minor);
    }
};

template<> struct Table<ucr::UcxWorker> {
    static constexpr const char* name = "ucx_worker";
    static constexpr const char* cols = "ticks,worker,context";
    static void row(std::FILE* f, const Stamped<ucr::UcxWorker>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.worker,
                     s.payload.context);
    }
};

template<> struct Table<ucr::UcxEndpoint> {
    static constexpr const char* name = "ucx_endpoint";
    static constexpr const char* cols = "ticks,endpoint,worker";
    static void                  row(std::FILE* f, const Stamped<ucr::UcxEndpoint>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.endpoint,
                                      s.payload.worker);
    }
};

// UcxDoor in words. Which entry point a row came through is not trivia: on MN5
// the MPI one-sided path calls only the `_nbi` doors and never the `_nbx` ones,
// so "which doors does this machine's stack actually drive" is a per-install
// fact a capture should state rather than require a readelf sweep to recover.
static const char* ucx_door_name(std::uint8_t raw) {
    switch (static_cast<ucr::UcxDoor>(raw)) {
        case ucr::UcxDoor::Nbx:
            return "nbx";
        case ucr::UcxDoor::Legacy:
            return "nb";
        case ucr::UcxDoor::LegacySync:
            return "sync_nb";
        case ucr::UcxDoor::Nbi:
            return "nbi";
        case ucr::UcxDoor::LegacyMsg:
            return "msg_recv_nb";
        case ucr::UcxDoor::LegacyPost:
            return "post";
    }
    return "?";
}

// StatusClass in words, for the reason ib_cq_origin_name and port_state_name
// exist: a raw 0/1/2 in a CSV is a value nobody checks. The three cases are the
// three things ucp_tag_send_nbx can return — it completed inline (nullptr), it
// failed, or it handed back a request that completes later — and telling them
// apart is the whole difference between "sent" and "queued".
static const char* ucx_status_class_name(std::uint8_t raw) {
    switch (static_cast<ucr::StatusClass>(raw)) {
        case ucr::StatusClass::Immediate:
            return "immediate";
        case ucr::StatusClass::Error:
            return "error";
        case ucr::StatusClass::Request:
            return "request";
    }
    return "?";
}

// `count` is the raw UCP datatype count and is NOT renamed to bytes here. It is
// one only for a contiguous byte datatype; the record deliberately does not
// claim otherwise (see ucx_records.hpp), and neither does this column.
template<> struct Table<ucr::UcxTagSend> {
    static constexpr const char* name = "ucx_tag_send";
    static constexpr const char* cols =
        "ticks,door,endpoint,buffer,request,count,tag,status,status_class,device";
    static void row(std::FILE* f, const Stamped<ucr::UcxTagSend>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%s,%u,%u,%u,%llu,0x%llx,%d,%s,%u",
                     (unsigned long long)s.temporal.a.ticks, ucx_door_name(s.payload.door),
                     s.payload.endpoint, s.payload.buffer, s.payload.request,
                     (unsigned long long)s.payload.count, (unsigned long long)s.payload.tag,
                     s.payload.status, ucx_status_class_name(s.payload.status_class),
                     (unsigned)s.payload.device);
    }
};

// A receive's `count` is the capacity OFFERED, and the column is named to say so
// rather than to look like the send side's. Summing it produces a number that
// means nothing: the message that lands may be smaller, later, or never.
template<> struct Table<ucr::UcxTagRecv> {
    static constexpr const char* name = "ucx_tag_recv";
    static constexpr const char* cols =
        "ticks,door,worker,buffer,request,capacity,tag,tag_mask,message,status,status_class";
    static void row(std::FILE* f, const Stamped<ucr::UcxTagRecv>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%s,%u,%u,%u,%llu,0x%llx,0x%llx,%u,%d,%s",
                     (unsigned long long)s.temporal.a.ticks, ucx_door_name(s.payload.door),
                     s.payload.worker, s.payload.buffer, s.payload.request,
                     (unsigned long long)s.payload.count, (unsigned long long)s.payload.tag,
                     (unsigned long long)s.payload.tag_mask, s.payload.message, s.payload.status,
                     ucx_status_class_name(s.payload.status_class));
    }
};

// RmaOp in words, for the reason every other discriminator in this file is: the
// direction is the only thing separating a put from a get, so a raw 0/1 hides
// the entire difference between "wrote to the peer" and "read from it".
static const char* ucx_rma_op_name(std::uint8_t raw) {
    switch (static_cast<ucr::RmaOp>(raw)) {
        case ucr::RmaOp::Put:
            return "put";
        case ucr::RmaOp::Get:
            return "get";
    }
    return "?";
}

template<> struct Table<ucr::UcxRma> {
    static constexpr const char* name = "ucx_rma";
    static constexpr const char* cols =
        "ticks,op,door,endpoint,buffer,request,rkey,count,remote_addr,status,status_class,device";
    static void row(std::FILE* f, const Stamped<ucr::UcxRma>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%s,%s,%u,%u,%u,%u,%llu,0x%llx,%d,%s,%u",
                     (unsigned long long)s.temporal.a.ticks, ucx_rma_op_name(s.payload.op),
                     ucx_door_name(s.payload.door), s.payload.endpoint, s.payload.buffer,
                     s.payload.request, s.payload.rkey, (unsigned long long)s.payload.count,
                     (unsigned long long)s.payload.remote_addr, s.payload.status,
                     ucx_status_class_name(s.payload.status_class), (unsigned)s.payload.device);
    }
};

template<> struct Table<ucr::UcxAmSend> {
    static constexpr const char* name = "ucx_am_send";
    static constexpr const char* cols =
        "ticks,endpoint,buffer,request,am_id,count,header_length,status,status_class,device";
    static void row(std::FILE* f, const Stamped<ucr::UcxAmSend>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%llu,%llu,%d,%s,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.endpoint, s.payload.buffer,
                     s.payload.request, s.payload.am_id, (unsigned long long)s.payload.count,
                     (unsigned long long)s.payload.header_length, s.payload.status,
                     ucx_status_class_name(s.payload.status_class), (unsigned)s.payload.device);
    }
};

// `resolved` rides as a word beside the values it qualifies, because "effective"
// and "requested" are different measurements and nothing else in the row says
// which one is present.
template<> struct Table<ucr::UcxMemMap> {
    static constexpr const char* name = "ucx_mem_map";
    static constexpr const char* cols = "ticks,memh,context,address,length,resolved";
    static void row(std::FILE* f, const Stamped<ucr::UcxMemMap>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%llx,%llu,%s", (unsigned long long)s.temporal.a.ticks,
                     s.payload.memh, s.payload.context, (unsigned long long)s.payload.address,
                     (unsigned long long)s.payload.length,
                     s.payload.resolved ? "effective" : "requested");
    }
};

template<> struct Table<ucr::UcxWorkerProgress> {
    static constexpr const char* name = "ucx_worker_progress";
    static constexpr const char* cols = "ticks,worker,progressed";
    static void                  row(std::FILE* f, const Stamped<ucr::UcxWorkerProgress>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.worker,
                                      s.payload.progressed);
    }
};

// The probe is where a tagged receive's identity comes from on the
// ucp_tag_msg_recv_nb path, and `length` is the one column in the whole UCX
// family that is a real byte count rather than a capacity. Join
// ucx_tag_recv.message -> ucx_tag_probe.message, taking the most recent probe at
// or before the receive: UCX recycles the handle, so a plain equality join pairs
// a receive with a dead probe once the pool wraps.
template<> struct Table<ucr::UcxTagProbe> {
    static constexpr const char* name = "ucx_tag_probe";
    static constexpr const char* cols =
        "ticks,worker,message,sender_tag,length,tag,tag_mask,removed";
    static void row(std::FILE* f, const Stamped<ucr::UcxTagProbe>& s,
                    const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%llx,%llu,0x%llx,0x%llx,%u",
                     (unsigned long long)s.temporal.a.ticks, s.payload.worker, s.payload.message,
                     (unsigned long long)s.payload.sender_tag, (unsigned long long)s.payload.length,
                     (unsigned long long)s.payload.tag, (unsigned long long)s.payload.tag_mask,
                     s.payload.removed);
    }
};

// UcxAtomicOp in words. The normalisation happened at the hook — see
// UcxAtomicOp for why the raw value could not be stored — so this only has to
// name the result.
static const char* ucx_atomic_op_name(std::uint8_t raw) {
    switch (static_cast<ucr::UcxAtomicOp>(raw)) {
        case ucr::UcxAtomicOp::Add:
            return "add";
        case ucr::UcxAtomicOp::And:
            return "and";
        case ucr::UcxAtomicOp::Or:
            return "or";
        case ucr::UcxAtomicOp::Xor:
            return "xor";
        case ucr::UcxAtomicOp::Swap:
            return "swap";
        case ucr::UcxAtomicOp::CompareSwap:
            return "cswap";
        case ucr::UcxAtomicOp::Unknown:
            return "unknown";
    }
    return "?";
}

template<> struct Table<ucr::UcxAtomic> {
    static constexpr const char* name = "ucx_atomic";
    static constexpr const char* cols =
        "ticks,op,fetching,door,endpoint,buffer,request,rkey,value,remote_addr,op_size,status,"
        "status_class";
    static void row(std::FILE* f, const Stamped<ucr::UcxAtomic>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%s,%u,%s,%u,%u,%u,%u,%llu,0x%llx,%llu,%d,%s",
                     (unsigned long long)s.temporal.a.ticks, ucx_atomic_op_name(s.payload.op),
                     s.payload.fetching, ucx_door_name(s.payload.door), s.payload.endpoint,
                     s.payload.buffer, s.payload.request, s.payload.rkey,
                     (unsigned long long)s.payload.value, (unsigned long long)s.payload.remote_addr,
                     (unsigned long long)s.payload.op_size, s.payload.status,
                     ucx_status_class_name(s.payload.status_class));
    }
};

// The two hashes travel together so a prefix collision is detectable: two
// workers sharing prefix_hash and differing in addr_hash are two workers the
// cross-rank join cannot separate, and a reader can say so rather than pick one.
template<> struct Table<ucr::UcxWorkerAddress> {
    static constexpr const char* name = "ucx_worker_address";
    static constexpr const char* cols = "ticks,worker,addr_hash,prefix_hash,addr_len";
    static void                  row(std::FILE* f, const Stamped<ucr::UcxWorkerAddress>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,0x%016llx,0x%016llx,%u", (unsigned long long)s.temporal.a.ticks,
                                      s.payload.worker, (unsigned long long)s.payload.addr_hash,
                                      (unsigned long long)s.payload.prefix_hash, s.payload.addr_len);
    }
};

// The only row in the UCX family that points OUTSIDE its own rank: join
// remote_prefix_hash to another capture's ucx_worker_address.prefix_hash to
// learn which peer an endpoint was dialled at.
template<> struct Table<ucr::UcxEpConnection> {
    static constexpr const char* name = "ucx_ep_connection";
    static constexpr const char* cols = "ticks,endpoint,remote_prefix_hash";
    static void                  row(std::FILE* f, const Stamped<ucr::UcxEpConnection>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,0x%016llx", (unsigned long long)s.temporal.a.ticks,
                                      s.payload.endpoint, (unsigned long long)s.payload.remote_prefix_hash);
    }
};

// The remote key an RMA or atomic row names. Before this record existed,
// ucx_rma.rkey and ucx_atomic.rkey interned a handle nothing ever created, so
// they joined to nothing at all. `endpoint` is what identifies the peer the key
// targets — join it on through ucx_ep_connection.
template<> struct Table<ucr::UcxRkey> {
    static constexpr const char* name = "ucx_rkey";
    static constexpr const char* cols = "ticks,rkey,endpoint";
    static void                  row(std::FILE* f, const Stamped<ucr::UcxRkey>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.rkey,
                     s.payload.endpoint);
    }
};

// The EXPORTING side: a registration packed for remote access. `buffer_hash`
// covers the whole packed buffer, which is possible here only because
// ucp_rkey_pack returns its size — ucp_ep_rkey_unpack does not, which is why
// ucx_rkey carries no counterpart to join this against. `buffer_len` is printed
// because a legitimate dummy key is 9 bytes and a reader seeing that number
// should not take it for a truncation.
template<> struct Table<ucr::UcxRkeyPack> {
    static constexpr const char* name = "ucx_rkey_pack";
    static constexpr const char* cols = "ticks,memh,context,buffer_hash,buffer_len";
    static void                  row(std::FILE* f, const Stamped<ucr::UcxRkeyPack>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,0x%016llx,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.memh, s.payload.context,
                     (unsigned long long)s.payload.buffer_hash, s.payload.buffer_len);
    }
};

// --- ucx_deep / UCT, 800-899 ------------------------------------------------
// UCX's transport layer. On the HPC-X/mlx5 stack these are the ONLY data-plane
// rows a capture has: ib_post_send and mlx5_device_send are both empty there,
// because UCX builds its WQE and rings its own doorbell inside a function
// nobody exports. See providers/ucx_deep/ucx_deep_records.hpp.

static const char* uct_op_name(std::uint8_t raw) {
    switch (static_cast<udr::UctOp>(raw)) {
        case udr::UctOp::AmShort:
            return "am_short";
        case udr::UctOp::AmShortIov:
            return "am_short_iov";
        case udr::UctOp::AmBcopy:
            return "am_bcopy";
        case udr::UctOp::AmZcopy:
            return "am_zcopy";
        case udr::UctOp::PutShort:
            return "put_short";
        case udr::UctOp::PutBcopy:
            return "put_bcopy";
        case udr::UctOp::PutZcopy:
            return "put_zcopy";
        case udr::UctOp::GetShort:
            return "get_short";
        case udr::UctOp::GetBcopy:
            return "get_bcopy";
        case udr::UctOp::GetZcopy:
            return "get_zcopy";
        case udr::UctOp::TagEagerShort:
            return "tag_eager_short";
        case udr::UctOp::TagEagerBcopy:
            return "tag_eager_bcopy";
        case udr::UctOp::TagEagerZcopy:
            return "tag_eager_zcopy";
        case udr::UctOp::TagRndvZcopy:
            return "tag_rndv_zcopy";
    }
    return "?";
}

// Whether the interface was instrumented, and if not, why. An empty uct_send
// has three causes and this column separates them: the guard refused, the
// transport has no data slots, or the workload sent nothing.
static const char* uct_instrumented_name(std::uint8_t raw) {
    switch (static_cast<udr::UctInstrumented>(raw)) {
        case udr::UctInstrumented::No:
            return "refused";
        case udr::UctInstrumented::Yes:
            return "yes";
        case udr::UctInstrumented::NoDataSlots:
            return "no_data_slots";
    }
    return "?";
}

// tl_name/dev_name are interned STRINGS, not pointers, so they resolve to
// "rc_mlx5"/"mlx5_0:1" rather than to a handle a reader would have to join.
// That pair is the fact this table exists for: it is the only thing in a
// capture that says whether a UCX message left the node.
template<> struct Table<udr::UctIface> {
    static constexpr const char* name = "uct_iface";
    static constexpr const char* cols =
        "ticks,iface,worker,md,tl_name,dev_name,open_mode,instrumented,patched_ops";
    static void row(std::FILE* f, const Stamped<udr::UctIface>& s,
                    const rec::InternDictionary& dict) {
        std::fprintf(f, "%llu,%u,%u,%u,%s,%s,0x%llx,%s,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.iface, s.payload.worker, s.payload.md, dict.str(s.payload.tl_name),
                     dict.str(s.payload.dev_name), (unsigned long long)s.payload.open_mode,
                     uct_instrumented_name(s.payload.instrumented), s.payload.patched_ops);
    }
};

// THE LANE. A ucp_ep fans out over several of these, possibly on different
// transports; it is not a queue pair and the record header says why it cannot
// be one.
template<> struct Table<udr::UctEndpoint> {
    static constexpr const char* name = "uct_ep";
    static constexpr const char* cols = "ticks,ep,iface";
    static void                  row(std::FILE* f, const Stamped<udr::UctEndpoint>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u", (unsigned long long)s.temporal.a.ticks, s.payload.ep,
                                      s.payload.iface);
    }
};

// `payload` MUST NOT be summed across op families. Am*/Put*/TagEager* left the
// node; Get* was requested and arrives later; TagRndvZcopy was advertised for
// the peer to pull and only `header` bytes went out. The op name is emitted
// first so a query that groups by it cannot avoid seeing the distinction.
template<> struct Table<udr::UctSend> {
    static constexpr const char* name = "uct_send";
    static constexpr const char* cols =
        "ticks,op,ep,iface,payload,header,remote_addr,tag,iovcnt,status";
    static void row(std::FILE* f, const Stamped<udr::UctSend>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%s,%u,%u,%llu,%llu,0x%llx,0x%llx,%u,%d",
                     (unsigned long long)s.temporal.a.ticks, uct_op_name(s.payload.op),
                     s.payload.ep, s.payload.iface, (unsigned long long)s.payload.payload,
                     (unsigned long long)s.payload.header,
                     (unsigned long long)s.payload.remote_addr, (unsigned long long)s.payload.tag,
                     s.payload.iovcnt, s.payload.status);
    }
};

// One row per process. Read it FIRST when uct_send is empty: accepted=0 means
// the provider refused to instrument this UCX, which is a different fact from
// an application that sent nothing.
template<> struct Table<udr::UctBinding> {
    static constexpr const char* name = "uct_binding";
    static constexpr const char* cols =
        "ticks,compiled_version,runtime_version,ops_size,accepted,forced";
    static void row(std::FILE* f, const Stamped<udr::UctBinding>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u.%u.%u,%u.%u.%u,%u,%u,%u", (unsigned long long)s.temporal.a.ticks,
                     s.payload.compiled_version >> 16, (s.payload.compiled_version >> 8) & 0xff,
                     s.payload.compiled_version & 0xff, s.payload.runtime_version >> 16,
                     (s.payload.runtime_version >> 8) & 0xff, s.payload.runtime_version & 0xff,
                     s.payload.ops_size, s.payload.accepted, s.payload.forced);
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
// --- NCCL host API, 900-999 -------------------------------------------------
// Every `ticks` here is the HOST CALL, not the collective. NCCL is
// stream-ordered: these rows say when the application ASKED, and the kernel ran
// later. Join to gpu_kernel for execution — see providers/nccl/nccl_records.hpp.

static const char* nccl_coll_op_name(std::uint8_t raw) {
    switch (static_cast<ncr::CollOp>(raw)) {
        case ncr::CollOp::AllReduce:
            return "all_reduce";
        case ncr::CollOp::Broadcast:
            return "broadcast";
        case ncr::CollOp::Reduce:
            return "reduce";
        case ncr::CollOp::AllGather:
            return "all_gather";
        case ncr::CollOp::ReduceScatter:
            return "reduce_scatter";
        case ncr::CollOp::AllToAll:
            return "all_to_all";
    }
    return "?";
}

// NCCL's own ncclDataType_t numbering, spelled out. The width matters — it is
// the only way `count` becomes a size — and a raw enum value would send every
// reader to nccl.h to find out that 7 is float32.
static const char* nccl_datatype_name(std::uint8_t raw) {
    switch (raw) {
        case 0:
            return "int8";
        case 1:
            return "uint8";
        case 2:
            return "int32";
        case 3:
            return "uint32";
        case 4:
            return "int64";
        case 5:
            return "uint64";
        case 6:
            return "float16";
        case 7:
            return "float32";
        case 8:
            return "float64";
        case 9:
            return "bfloat16";
        case 10:
            return "fp8_e4m3";
        case 11:
            return "fp8_e5m2";
        default:
            return "?";
    }
}

// Bytes per element, so `count` can be turned into a size by whoever needs one.
// Returns 0 for a datatype this build does not know, and 0 is the honest answer
// there: a guessed width would silently scale every derived number.
static std::uint64_t nccl_datatype_bytes(std::uint8_t raw) {
    switch (raw) {
        case 0:
        case 1:
        case 10:
        case 11:
            return 1;
        case 6:
        case 9:
            return 2;
        case 2:
        case 3:
        case 7:
            return 4;
        case 4:
        case 5:
        case 8:
            return 8;
        default:
            return 0;
    }
}

static const char* nccl_redop_name(std::uint8_t raw) {
    switch (raw) {
        case 0:
            return "sum";
        case 1:
            return "prod";
        case 2:
            return "max";
        case 3:
            return "min";
        case 4:
            return "avg";
        case 0xFF:
            return "";  // the operation does not reduce
        default:
            return "?";
    }
}

// `nccl_rank`, not `rank`. The export prepends the CAPTURE's rank to every
// table, so a payload field called `rank` would produce two columns of that
// name and every query against this table would silently take whichever the
// engine picked. They are also not the same number: a process is one rank in
// the capture and may hold a different rank inside a communicator it joined —
// which is the whole reason NCCL's rank is worth recording separately.
//
// `nccl_comm_hash` is NCCL's own hash of the same identity and is the join to
// the profiler-plugin tables — NOT a duplicate of `comm_hash`, which is ours and
// joins rank to rank. 0 means not derivable (ncclCommSplit, grow), so a join on
// it must exclude 0.
template<> struct Table<ncr::NcclComm> {
    static constexpr const char* name = "nccl_comm";
    static constexpr const char* cols =
        "ticks,comm,comm_hash,nccl_comm_hash,nccl_rank,nranks,cuda_dev";
    static void row(std::FILE* f, const Stamped<ncr::NcclComm>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%llu,%llu,%d,%d,%d", (unsigned long long)s.temporal.a.ticks,
                     s.payload.comm, (unsigned long long)s.payload.comm_hash,
                     (unsigned long long)s.payload.nccl_comm_hash, s.payload.rank, s.payload.nranks,
                     s.payload.cuda_dev);
    }
};

// `logical_bytes` is count x the datatype width and is named to say so. It is
// what the application handed over, NOT what crosses the wire: a ring
// all-reduce moves about 2x(n-1)/n of it per rank, a broadcast moves it once.
// The column exists because every reader would otherwise recompute it, and it
// is named `logical_` because the first reader to call it `bytes` would put it
// on a bandwidth axis.
template<> struct Table<ncr::NcclCollective> {
    static constexpr const char* name = "nccl_collective";
    static constexpr const char* cols =
        "ticks,op,comm,sendbuff,recvbuff,stream,count,datatype,elem_bytes,logical_bytes,redop,root";
    static void row(std::FILE* f, const Stamped<ncr::NcclCollective>& s,
                    const rec::InternDictionary&) {
        const auto w = nccl_datatype_bytes(s.payload.datatype);
        std::fprintf(f, "%llu,%s,%u,%u,%u,%u,%llu,%s,%llu,%llu,%s,%d",
                     (unsigned long long)s.temporal.a.ticks, nccl_coll_op_name(s.payload.op),
                     s.payload.comm, s.payload.sendbuff, s.payload.recvbuff, s.payload.stream,
                     (unsigned long long)s.payload.count, nccl_datatype_name(s.payload.datatype),
                     (unsigned long long)w, (unsigned long long)(s.payload.count * w),
                     nccl_redop_name(s.payload.redop), s.payload.root);
    }
};

template<> struct Table<ncr::NcclP2p> {
    static constexpr const char* name = "nccl_p2p";
    static constexpr const char* cols =
        "ticks,dir,comm,peer,buffer,stream,count,datatype,elem_bytes,logical_bytes";
    static void row(std::FILE* f, const Stamped<ncr::NcclP2p>& s, const rec::InternDictionary&) {
        const auto w = nccl_datatype_bytes(s.payload.datatype);
        std::fprintf(f, "%llu,%s,%u,%d,%u,%u,%llu,%s,%llu,%llu",
                     (unsigned long long)s.temporal.a.ticks,
                     static_cast<ncr::P2pDir>(s.payload.dir) == ncr::P2pDir::Send ? "send" : "recv",
                     s.payload.comm, s.payload.peer, s.payload.buffer, s.payload.stream,
                     (unsigned long long)s.payload.count, nccl_datatype_name(s.payload.datatype),
                     (unsigned long long)w, (unsigned long long)(s.payload.count * w));
    }
};

template<> struct Table<ncr::NcclGroup> {
    static constexpr const char* name = "nccl_group";
    static constexpr const char* cols = "ticks,edge,depth";
    static void row(std::FILE* f, const Stamped<ncr::NcclGroup>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%s,%u", (unsigned long long)s.temporal.a.ticks,
                     static_cast<ncr::GroupEdge>(s.payload.edge) == ncr::GroupEdge::Start ? "start"
                                                                                          : "end",
                     s.payload.depth);
    }
};

template<> struct Table<ncr::NcclWindow> {
    static constexpr const char* name = "nccl_window";
    static constexpr const char* cols = "ticks,window,comm,buffer,size,win_flags";
    static void row(std::FILE* f, const Stamped<ncr::NcclWindow>& s, const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u,%u,%u,%llu,%d", (unsigned long long)s.temporal.a.ticks,
                     s.payload.window, s.payload.comm, s.payload.buffer,
                     (unsigned long long)s.payload.size, s.payload.win_flags);
    }
};

template<> struct Table<ncr::NcclDevComm> {
    static constexpr const char* name = "nccl_dev_comm";
    static constexpr const char* cols = "ticks,comm";
    static void                  row(std::FILE* f, const Stamped<ncr::NcclDevComm>& s,
                                     const rec::InternDictionary&) {
        std::fprintf(f, "%llu,%u", (unsigned long long)s.temporal.a.ticks, s.payload.comm);
    }
};

// --- the profiler-plugin tables ---------------------------------------------
//
// `nccl_comm_hash` is emitted as an unsigned decimal on every one of these, the
// same width and signedness as on nccl_comm, because it is the join key across
// all four and a column that changes representation between tables is a join
// that silently returns nothing.

template<> struct Table<ncr::NcclProfComm> {
    static constexpr const char* name = "nccl_prof_comm";
    static constexpr const char* cols = "ticks,nccl_comm_hash,comm_name,nnodes,nranks,nccl_rank";
    static void                  row(std::FILE* f, const Stamped<ncr::NcclProfComm>& s,
                                     const rec::InternDictionary& in) {
        std::fprintf(f, "%llu,%llu,%s,%d,%d,%d", (unsigned long long)s.temporal.a.ticks,
                     (unsigned long long)s.payload.nccl_comm_hash,
                     csv_quoted(s.payload.comm_name ? in.str(s.payload.comm_name) : "").c_str(),
                     s.payload.nnodes, s.payload.nranks, s.payload.rank);
    }
};

// `algo` and `proto` are the reason this table exists, and they are emitted as
// the strings NCCL gave rather than as handles: a reader asking "what algorithm
// did this run use" should be able to GROUP BY the answer without joining to the
// intern dictionary first.
//
// `ticks` and `end_ticks` are BOTH emitted, which no host-API nccl table does.
// There the interval is an enqueue and its width means nothing useful; here it
// is the collective's own lifetime inside NCCL, so the duration is a fact worth
// having and dropping the end would throw it away.
//
// `planned_bytes` is deliberately ABSENT. It would have to come from the proxy
// ops, and summing them here would present a plan as a measurement — see
// nccl_proxy_op below.
template<> struct Table<ncr::NcclCollExec> {
    static constexpr const char* name = "nccl_coll_exec";
    static constexpr const char* cols = "ticks,end_ticks,nccl_comm_hash,event_id,parent_event_id,"
                                        "func,algo,proto,datatype,count,root,n_channels,n_warps,"
                                        "seq_number";
    static void row(std::FILE* f, const Stamped<ncr::NcclCollExec>& s,
                    const rec::InternDictionary& in) {
        auto t = [&in](core::Handle h) { return csv_quoted(h ? in.str(h) : ""); };
        std::fprintf(f, "%llu,%llu,%llu,%llu,%llu,%s,%s,%s,%s,%llu,%d,%u,%u,%llu",
                     (unsigned long long)s.temporal.a.ticks, (unsigned long long)s.temporal.b.ticks,
                     (unsigned long long)s.payload.nccl_comm_hash,
                     (unsigned long long)s.payload.event_id,
                     (unsigned long long)s.payload.parent_event_id, t(s.payload.func).c_str(),
                     t(s.payload.algo).c_str(), t(s.payload.proto).c_str(),
                     t(s.payload.datatype).c_str(), (unsigned long long)s.payload.count,
                     s.payload.root, s.payload.n_channels, s.payload.n_warps,
                     (unsigned long long)s.payload.seq_number);
    }
};

// `planned_bytes` is n_steps x chunk_size, and the name is the whole point: it
// is what NCCL SCHEDULED, not what a completion confirmed. The column exists
// because every reader would otherwise multiply the two, and it is named
// `planned_` because the first reader to call it `bytes` would put it on a
// bandwidth axis next to ib_post_send's byte counts, which ARE completions.
template<> struct Table<ncr::NcclProxyOp> {
    static constexpr const char* name = "nccl_proxy_op";
    static constexpr const char* cols = "ticks,end_ticks,event_id,parent_event_id,dir,peer,"
                                        "channel_id,n_steps,chunk_size,planned_bytes,pid";
    static void row(std::FILE* f, const Stamped<ncr::NcclProxyOp>& s,
                    const rec::InternDictionary&) {
        const auto planned =
            static_cast<long long>(s.payload.n_steps) * static_cast<long long>(s.payload.chunk_size);
        std::fprintf(f, "%llu,%llu,%llu,%llu,%s,%d,%u,%d,%d,%lld,%d",
                     (unsigned long long)s.temporal.a.ticks, (unsigned long long)s.temporal.b.ticks,
                     (unsigned long long)s.payload.event_id,
                     (unsigned long long)s.payload.parent_event_id,
                     s.payload.is_send ? "send" : "recv", s.payload.peer,
                     (unsigned)s.payload.channel_id, s.payload.n_steps, s.payload.chunk_size,
                     planned, s.payload.pid);
    }
};

// known_types() is a set, and --types sorts by id.
using TableRecords =
    std::tuple<MemObject, EntityClose, GpuAccess, GpuKernel, CudaMemcpy, SourceLine, NodeIdentity,
               GpuDevice, Invocation, PeerMapping,
               ClockAnchor, CaptureStatus, Attribution, ibr::IbQueuePair, ibr::IbCompletionQueue,
               ibr::IbModifyQp,
               ibr::IbPostSend, ibr::IbSharedRecvQueue, ibr::IbPostRecv, ibr::IbAddressHandle,
               ibr::IbDevice, ibr::IbReqNotifyCq, ibr::IbPollCq, ibr::IbMemRegion, ibr::IbRecvCqe,
               ibr::IbQpSignalling, ibr::IbPostSendSg, ibr::IbPort, mxr::Mlx5Umem, mxr::Mlx5Uar,
               mxr::Mlx5DevxCmd, mxr::Mlx5DevxQp, mxr::Mlx5DevxQpConnect, mxr::Mlx5QueuePair,
               mxr::Mlx5Doorbell, mxr::Mlx5DeviceSend, ucr::UcxContext, ucr::UcxWorker,
               ucr::UcxEndpoint, ucr::UcxTagSend, ucr::UcxTagRecv, ucr::UcxRma, ucr::UcxAmSend,
               ucr::UcxMemMap, ucr::UcxWorkerProgress, ucr::UcxTagProbe, ucr::UcxAtomic,
               ucr::UcxWorkerAddress, ucr::UcxEpConnection, ucr::UcxRkey, ucr::UcxRkeyPack,
               udr::UctBinding, udr::UctIface,
               udr::UctEndpoint, udr::UctSend, ncr::NcclComm, ncr::NcclCollective, ncr::NcclP2p,
               ncr::NcclGroup, ncr::NcclWindow, ncr::NcclDevComm, ncr::NcclProfComm,
               ncr::NcclCollExec, ncr::NcclProxyOp>;

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
        std::fprintf(stderr, "pcomm: cannot use %s as an output directory: %s\n", outdir.c_str(),
                     ec ? ec.message().c_str() : "not a directory");
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

    // The dictionary rides along as a table. `--csv` resolves handles inline in
    // its printers; Parquet deliberately does not decode, which left the
    // derived layer able to group by an interned column but not to NAME one —
    // "uct_iface grouped by tl_name = 4123" is not an answer about a transport.
    const std::size_t intern_rows = capture::load_intern_text(con, ranks);
    // What the files themselves look like, written beside the records they
    // frame. This is the only table in an export that describes the EXPORT
    // rather than the run, and it exists because a damaged capture used to
    // produce a Parquet directory indistinguishable from a healthy one.
    const std::size_t integrity_rows = capture::load_capture_integrity(con, ranks);

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

    if (intern_rows) {
        const auto out = (outdir / (std::string(duck::kInternTextTable) + ".parquet")).string();
        if (duck_exec(con,
                      "COPY \"" + std::string(duck::kInternTextTable) + "\" TO '" + out +
                          "' (FORMAT PARQUET)",
                      duck::kInternTextTable)) {
            tables++;
            rows_total += intern_rows;
        } else {
            failed++;
        }
    }

    // UNCONDITIONAL, unlike the dictionary above. An export with no
    // capture_integrity is one a reader cannot judge, and "the capture was so
    // broken there were no ranks to describe" is exactly when it must not be
    // silently omitted. A zero-row table still says the export ran.
    {
        const auto out =
            (outdir / (std::string(duck::kCaptureIntegrityTable) + ".parquet")).string();
        if (duck_exec(con,
                      "COPY \"" + std::string(duck::kCaptureIntegrityTable) + "\" TO '" + out +
                          "' (FORMAT PARQUET)",
                      duck::kCaptureIntegrityTable)) {
            tables++;
            rows_total += integrity_rows;
        } else {
            failed++;
        }
    }

    duckdb_disconnect(&con);
    duckdb_close(&db);

    std::printf("pcomm: %zu ranks, %zu tables, %zu rows -> %s/*.parquet%s\n", ranks.size(), tables,
                rows_total, outdir.c_str(), failed ? " (SOME FAILED, see above)" : "");
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
        std::fprintf(stderr,
                     "pcomm: no raw .parquet tables in %s — run `pcomm export --parquet` first\n",
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
        // The declared column contract, checked here too and not only in
        // link::define(): this is the path that writes the fixture the suite,
        // the agent and every other tool read, so it is the one where a renamed
        // column does the most damage before anyone notices.
        if (!link::columns_match(con, t)) {
            failed++;
            continue;
        }
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
    //
    // THE TABLE IS NOT A TABLE HERE, and that is why this used to print
    // nothing — ever. `link` COPYs each kTables entry straight to Parquet; it
    // never CREATEs any of them as a relation, so `FROM link_health` resolved
    // against a catalog that has no such name and the query failed with
    // "Catalog Error: Table with name link_health does not exist!". The error
    // went into a duckdb_result nobody inspected, so a run with findings and a
    // run without looked identical. Found on kt-combined-44546852, where
    // link_health holds wr_flight_unretired = 56656 and `pcomm link` reported a
    // clean run.
    //
    // So the query runs against the same SQL that was just written, by name
    // lookup into kTables rather than by catalog lookup. Reading it through
    // duckdb_fetch_chunk rather than duckdb_row_count/duckdb_value_varchar is
    // the second half: those are the deprecated row API, and this is the one
    // that is not scheduled for removal.
    const char* health_sql = nullptr;
    for (const auto& t : link::kTables)
        if (std::string_view(t.name) == "link_health")
            health_sql = t.sql;

    // Severity is carried into the line rather than used to filter it. A 'note'
    // row is exactly the context a reader needs when a 'defect' row above it
    // looks alarming — "69,470 unretired" reads very differently once the line
    // beneath it says the queue pairs declared they would not signal. Printing
    // only defects would reproduce, on this surface, the same missing-context
    // failure the severity column was added to fix.
    duckdb_result res;
    if (health_sql && duckdb_query(con,
                                   ("SELECT finding::VARCHAR, n::VARCHAR, severity::VARCHAR FROM (" +
                                    std::string(health_sql) +
                                    ") WHERE n > 0 ORDER BY severity, finding")
                                       .c_str(),
                                   &res) == DuckDBSuccess) {
        auto text = [](duckdb_string_t s) {
            return duckdb_string_is_inlined(s)
                       ? std::string(s.value.inlined.inlined, s.value.inlined.length)
                       : std::string(s.value.pointer.ptr, s.value.pointer.length);
        };
        while (duckdb_data_chunk chunk = duckdb_fetch_chunk(res)) {
            const idx_t rows = duckdb_data_chunk_get_size(chunk);
            auto*       fv = static_cast<duckdb_string_t*>(
                duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0)));
            auto* nv = static_cast<duckdb_string_t*>(
                duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 1)));
            auto* sv = static_cast<duckdb_string_t*>(
                duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 2)));
            for (idx_t row = 0; row < rows; ++row) {
                // Reported, not counted as a failure. `failed` drives the exit
                // code and the "SOME FAILED" banner, and those mean a query did
                // not run. A finding means a query ran and found something —
                // including things that are properties of the capture rather
                // than of the link, like a run that stopped with work in
                // flight. That last sentence is now a COLUMN rather than a
                // caveat in a comment nobody printing this line will read.
                std::fprintf(stderr, "pcomm: link_health [%s]: %s = %s\n", text(sv[row]).c_str(),
                             text(fv[row]).c_str(), text(nv[row]).c_str());
            }
            duckdb_destroy_data_chunk(&chunk);
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

    // BOUND TO A VIEW, NOT PARENTHESISED, and that is the whole of finding 13.
    //
    // This used to be `COPY (<query>) TO`, which is the obvious spelling and
    // turns two ordinary things a person writes into parse errors:
    //
    //   SELECT 1;         -> COPY (SELECT 1;) TO ...    syntax error at ';'
    //   SELECT 1 -- why   -> the comment eats the wrapper's own ')', and the
    //                        error reads "syntax error at end of input", which
    //                        points at nothing at all
    //
    // The second is the one that hurts: a trailing comment is how every .sql
    // file under tests/hpc/*/asserts is annotated, and sql_check.sh carries a
    // python splicer whose first job is to strip them back off before calling
    // this command. Four of four application studies wrote their own assertion
    // runner rather than use that harness.
    //
    // TEXTUAL NORMALISATION WAS TRIED FIRST AND ABANDONED. Stripping trailing
    // semicolons and putting the ')' on its own line fixes both cases above and
    // still fails on `SELECT 1; -- why`, where the ';' is no longer last. Going
    // further means deciding whether a '--' is a comment or two characters
    // inside a string literal, which is lexing — and a heuristic lexer that
    // corrupts `SELECT 'a--b'` would be a far worse defect than the one it fixes.
    //
    // So DuckDB does the lexing. As a view BODY the query sits at statement
    // level, where a trailing ';' terminates it and a trailing comment is just
    // a comment. No guessing, and the accepted grammar becomes "whatever DuckDB
    // accepts" instead of a rule this file has to maintain.
    //
    // NO FALLBACK TO THE OLD WRAPPER, because measurement said it would never
    // fire. `COPY (<q>)` and a view body accept the same select-statement
    // grammar: DESCRIBE, SHOW and SUMMARIZE are refused by BOTH (probed on
    // du04), so a fallback would have been dead code that the next reader has to
    // reason about. Those three are worth supporting one day; neither spelling
    // supports them today and pretending otherwise helps nobody.
    //
    // The query goes on its OWN LINE so DuckDB's error text quotes what the
    // user typed, with the caret under the user's column, instead of a line
    // beginning `CREATE OR REPLACE TEMP VIEW _pcomm_query AS`. The reported
    // LINE number is one higher than the user's for the same reason; the quoted
    // text and the caret are what a person reads, and both are now theirs.
    //
    // One documented consequence: a result with duplicate output column names
    // comes back as `x, x_1` where the old wrapper emitted `x, x`, because a
    // view names its columns. That is the header for `SELECT a.*, b.*` becoming
    // unambiguous, which is an improvement, but it IS a change.
    constexpr const char* kQueryView = "_pcomm_query";
    const std::string     bind =
        "CREATE OR REPLACE TEMP VIEW " + std::string(kQueryView) + " AS\n" + query;

    const auto tmp =
        fs::temp_directory_path() / ("pcomm-sql-" + std::to_string(::getpid()) + ".csv");

    // Via a temp file, NOT `COPY ... TO '/dev/stdout'`. That spelling looks
    // tidier and is wrong: when stdout is a redirected file DuckDB opens it and
    // seeks to offset 0, silently overwriting whatever the process already
    // wrote. Two `sql` runs into one redirect lost the first, and so would any
    // `pcomm sql ... > out.csv` that printed anything beforehand.
    const bool ok =
        duck_exec(con, bind, "query") &&
        duck_exec(con,
                  "COPY (SELECT * FROM " + std::string(kQueryView) + ") TO '" + tmp.string() +
                      "' (FORMAT CSV, HEADER)",
                  "query");
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
    //
    // Deliberately Interval *only*. A Window also has both endpoints populated,
    // but they bound uncertainty rather than measuring a duration — returning
    // one here would assert the event took that long, which is exactly the false
    // claim this distinction exists to prevent. The derived layer carries the
    // kind through to timeline_span for the same reason; a renderer that wants
    // to draw a window has to opt in to drawing it AS a window.
    double dur_ns(const core::Temporal& t) const {
        return (t.kind == core::TimeKind::Interval && t.b.ticks > t.a.ticks)
                   ? (t.b.ticks - t.a.ticks) * ns_per_tick
                   : 0.0;
    }
};

// Fits RankClocks per (rank, DOMAIN) and picks the earliest anchored wall-ns as
// the shared origin (0 if no rank has anchors).
//
// PER DOMAIN, and that is not defensive coding — a capture with kernel timing
// has FOUR anchors per rank, not two: domain 0 (TSC) at start and end, and
// domain 1 (CUPTI's CLOCK_REALTIME ns) nested between them. A fit that took
// front() and back() and ignored `domain` would be reading a rate across two
// unrelated clocks, and the only reason it produced the right answer on this
// capture is that the engine happens to write the domain-0 pair outermost.
// Nothing guarantees that order, and a domain-1 tick pushed through the TSC
// fit lands ~1.8e18 ns away — the same class of error `_clock` avoids by
// joining on (rank, domain), which is why link_sql.hpp has never had this bug.
//
// Domains a capture never anchors are simply absent from the map; a caller
// asking for one gets `clock_for`'s identity fallback and can say so.
struct RankClocks {
    // Small and linear rather than a hash: a rank has at most a handful of
    // domains (0, 1, and 2+gpu ordinal), so lookup is a two-element scan.
    std::vector<std::pair<std::uint16_t, RankClock>> by_domain;

    const RankClock* find(std::uint16_t domain) const {
        for (const auto& [d, c] : by_domain)
            if (d == domain)
                return &c;
        return nullptr;
    }
};

std::pair<std::vector<RankClocks>, std::uint64_t> build_clocks(const std::vector<RankDir>& ranks) {
    std::vector<RankClocks> clk(ranks.size());
    std::uint64_t           origin = ~0ull;
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        auto anchors = load_stream<ClockAnchor>(stream_path<ClockAnchor>(ranks[i]));
        // First and last anchor PER DOMAIN, in stream order.
        std::vector<std::pair<std::uint16_t, std::pair<ClockAnchor, ClockAnchor>>> ends;
        for (const auto& s : anchors) {
            const auto d = s.payload.domain;
            auto       it =
                std::find_if(ends.begin(), ends.end(), [d](const auto& e) { return e.first == d; });
            if (it == ends.end())
                ends.push_back({d, {s.payload, s.payload}});
            else
                it->second.second = s.payload;
        }
        for (const auto& [d, ab] : ends) {
            const auto& [a, b] = ab;
            RankClock c;
            c.t0 = a.local_ticks;
            c.w0 = a.wall_ns;
            long double dt = static_cast<long double>(b.local_ticks) - a.local_ticks;
            if (dt > 0)
                c.ns_per_tick =
                    static_cast<double>((static_cast<long double>(b.wall_ns) - a.wall_ns) / dt);
            clk[i].by_domain.push_back({d, c});
            origin = std::min(origin, static_cast<std::uint64_t>(a.wall_ns));
        }
    }
    if (origin == ~0ull)
        origin = 0;
    return {std::move(clk), origin};
}

// The clock a record should be read through: its own domain's, or — when the
// capture anchored no such domain — an identity clock, so the caller still
// gets a number rather than a crash. Callers that care count the misses.
inline const RankClock& clock_for(const RankClocks& rc, std::uint16_t domain,
                                  std::size_t& unanchored) {
    static const RankClock identity{};
    if (const auto* c = rc.find(domain))
        return *c;
    unanchored++;
    return identity;
}

// --- one trace model, two encodings ------------------------------------------
//
// WHY THIS EXISTS. The Chrome-JSON and protobuf exporters used to answer the
// same question in two hand-written passes, and they drifted exactly as far as
// that invites: the JSON grew `args` the pftrace never had, the pftrace grew a
// kernel lane and send->completion flows the JSON never had, and one capture
// produced two traces that disagreed about what was in it. Neither drift was a
// decision; both were a second pass nobody updated.
//
// So the model is built ONCE — clocks applied, work requests paired, kernels
// filtered, lanes ordered, tracks reduced to the ones that carry something —
// and the two encoders below choose syntax and nothing else. A future exporter
// is a third encoder, not a third pass.
//
// This is the OPPOSITE call to the one made for _wr_flight in link_sql.hpp,
// and deliberately: that rule is stated twice because its two callers live in
// different worlds (one must run without DuckDB, one runs over Parquet) and
// neither can adopt the other. These two callers live in the same file, take
// the same input and run in the same build. Same reasoning, opposite answer.

enum class Lane : std::uint8_t { Send, Complete, Kernel };

// The endpoint id for the kernels lane. Real queue pairs are 24-bit in
// practice, but the sentinel is picked from the top of the range rather than
// from below so it cannot collide with a qp_num on any device.
constexpr std::uint32_t KERNEL_EP = 0xFFFFFFFFu;

// A slice with nothing left to resolve: no record lookup, no clock arithmetic,
// no intern dictionary. Everything an encoder could want to write is here,
// which is what stops one encoder from carrying a field the other forgot.
struct Slice {
    std::uint64_t t_ns{};
    std::uint64_t dur_ns{};
    int           rank{};
    std::uint32_t ep{};
    Lane          lane{};
    const char*   name{};
    std::uint64_t flow{};      // 0 = not part of a flow
    bool          flow_end{};  // true on the completion that closes it
    // The annotation payload — what makes a slice worth clicking on.
    std::uint64_t wr_id{};
    std::uint64_t remote_addr{};
    std::uint32_t bytes{};
    std::uint32_t rkey{};
    std::uint8_t  opcode{};
    std::uint8_t  status{};
};

// Bandwidth over time — the question a comm trace gets opened to answer that a
// pile of slices cannot. A 4-byte send and a 4-MB send are the same mark on a
// timeline and are not the same event.
enum class Counter : std::uint8_t { BytesSent, SendRate };

// SAMPLED ON A WINDOW, NOT PER EVENT, and the arithmetic is the reason. A
// counter sample per send would add one packet per send — 5.8 M on
// scale_hubspoke_ib_kt-44552862, a 25% packet increase to draw a line whose
// screen resolution is a few thousand pixels. The window is chosen per rank
// from its own extent so the sample count is bounded no matter how long the
// run: TARGET_SAMPLES per rank, floored so a short capture does not sample at
// nanosecond resolution.
constexpr std::size_t   COUNTER_TARGET_SAMPLES = 2048;
constexpr std::uint64_t COUNTER_MIN_WINDOW_NS = 1'000'000;  // 1 ms

struct TraceStats {
    std::size_t slices{};
    std::size_t flows{};
    std::size_t unmatched{};
    std::size_t unanchored{};
    std::size_t counters{};
    std::size_t unsignalled_qps{};    // queue pairs refused for pairing
    std::size_t unsignalled_sends{};  // sends on them, left with no flow
};

// Walk the capture and hand the caller, per rank: the rank, then each lane it
// actually uses, then every slice in TIME ORDER.
//
// The ordering is not a courtesy to the encoders, it is load-bearing twice
// over. Flow matching needs it, because a completion can only retire a send
// already posted and reading the poll_cq stream whole after the post_send
// stream whole guarantees the reverse. And the protobuf encoder needs it,
// because Perfetto asks for monotonic timestamps within a sequence.
template<class OnRank, class OnLane, class OnSlice, class OnCounter>
TraceStats build_trace(const std::vector<RankDir>& ranks, const std::vector<RankClocks>& clk,
                       std::uint64_t origin, OnRank on_rank, OnLane on_lane, OnSlice on_slice,
                       OnCounter on_counter) {
    TraceStats st;
    // Each record through ITS OWN domain's fit — see build_clocks. A verbs
    // record is domain 0; a kernel record is not, and pushing one through the
    // other's rate is how a timeline becomes fiction.
    auto ns = [&](const RankClocks& rc, const core::Temporal& t) {
        return clock_for(rc, t.a.domain, st.unanchored).wall_at(t.a.ticks) - origin;
    };
    auto dur = [&](const RankClocks& rc, const core::Temporal& t) {
        return static_cast<std::uint64_t>(clock_for(rc, t.a.domain, st.unanchored).dur_ns(t));
    };

    // A completion names the work request it retires by (qp_num, wr_id) — the
    // same key ib_post_send stamped when it posted. The map holds a QUEUE per
    // key rather than one entry because wr_id is caller-chosen and routinely
    // recycled as a buffer index: completions on a queue pair retire in the
    // order they were posted, so first-in-first-out matches the right
    // generation under reuse instead of overwriting it.
    //
    // The same rule is stated in SQL as _wr_flight in link_sql.hpp, and
    // tests/wr_flow_rule.sh is what keeps the two saying the same thing.
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::deque<std::uint64_t>> pending;
    std::uint64_t                                                                next_flow = 1;

    // Receive completions carry the RECEIVER's wr_id, which names an
    // ib_post_recv and never a send. Pairing without checking this bit links a
    // send to whatever unrelated receive happened to reuse the number.
    constexpr std::uint8_t WC_RECV_BIT = 0x80;

    struct Ev {
        std::uint64_t t_ns;
        std::uint64_t dur_ns;
        std::uint32_t idx;
        Lane          lane;
    };

    for (std::size_t i = 0; i < ranks.size(); ++i) {
        const auto& r = ranks[i];
        const auto& c = clk[i];

        auto sends = load_stream<ibr::IbPostSend>(stream_path<ibr::IbPostSend>(r));
        auto polls = load_stream<ibr::IbPollCq>(stream_path<ibr::IbPollCq>(r));
        // Compute, beside the comm. Without this a trace can show that a rank
        // stopped sending and can never say whether it was working — which is
        // the whole reason gpu_kernel exists.
        //
        // source = 2 (HostSubmit) is excluded for the reason _tl_raw excludes
        // it: those rows are the launch API call, not the execution, and they
        // trail the real thing by milliseconds. They must not reach a lane
        // labelled "kernels".
        auto kernels = load_stream<GpuKernel>(stream_path<GpuKernel>(r));

        std::vector<Ev> evs;
        evs.reserve(sends.size() + polls.size() + kernels.size());
        for (std::size_t k = 0; k < sends.size(); ++k)
            evs.push_back({ns(c, sends[k].temporal), dur(c, sends[k].temporal),
                           static_cast<std::uint32_t>(k), Lane::Send});
        for (std::size_t k = 0; k < polls.size(); ++k)
            evs.push_back({ns(c, polls[k].temporal), dur(c, polls[k].temporal),
                           static_cast<std::uint32_t>(k), Lane::Complete});
        for (std::size_t k = 0; k < kernels.size(); ++k)
            if (kernels[k].payload.source != 2)
                evs.push_back({ns(c, kernels[k].temporal), dur(c, kernels[k].temporal),
                               static_cast<std::uint32_t>(k), Lane::Kernel});
        std::stable_sort(evs.begin(), evs.end(),
                         [](const Ev& a, const Ev& b) { return a.t_ns < b.t_ns; });

        // Only lanes that carry something get declared. Declaring one per
        // ib_queue_pair row instead put 13,461 empty lanes into a 14,387-lane
        // capture — 93% of the track list, and the reason the kernels lane was
        // impossible to find in it.
        // WHICH QUEUE PAIRS MAY BE PAIRED AT ALL.
        //
        // FIFO matching is exact — not a heuristic — for one specific reason:
        // a reliable-connected QP returns completions IN POSTING ORDER, which
        // is a transport guarantee. So the Nth completion retires the Nth send
        // and `wr_id` need not be unique. NVSHMEM posts every one of its 5.8 M
        // work requests with wr_id = 1 and the pairing is still exact.
        //
        // THAT HOLDS ONLY IF EVERY WORK REQUEST IS SIGNALLED. An unsignalled WR
        // produces no CQE; the next signalled completion retires it silently
        // along with itself. Pairing Nth-to-Nth across those is not slightly
        // off, it is wrong: it attributes a completion to a send that is not
        // the one it retired, and prints a latency for it.
        //
        // Measured, which is why this guard exists: scale_hubspoke_ib_kt-
        // 44552862 has equal counts on all 862 queue pairs and pairs exactly,
        // while kt-combined-44546852 has 3,570 sends against 1,800 completions
        // on every queue pair — NCCL signalling roughly every second WR. The
        // second capture had been producing confident, wrong latencies.
        //
        // The capture cannot tell us WHICH WRs were signalled: ib_post_send
        // carries no send_flags and ib_queue_pair no sq_sig_all. Counts are the
        // only evidence available, so an inequality means "cannot pair this
        // queue pair" rather than "pair it approximately". A hole you can see
        // beats a number that looks like a measurement.
        std::map<std::uint32_t, std::size_t> n_send, n_comp;
        for (const auto& s : sends)
            n_send[s.payload.qp_num]++;
        for (const auto& p : polls)
            if (!(p.payload.opcode & 0x80))
                n_comp[p.payload.qp_num]++;
        std::set<std::uint32_t> pairable;
        for (const auto& [qp, ns_] : n_send) {
            const auto it = n_comp.find(qp);
            if (it != n_comp.end() && it->second == ns_)
                pairable.insert(qp);
            else {
                st.unsignalled_qps++;
                st.unsignalled_sends += ns_;
            }
        }

        std::set<std::uint32_t> live_qps;
        bool                    any_kernel = false;
        for (const auto& e : evs) {
            if (e.lane == Lane::Send)
                live_qps.insert(sends[e.idx].payload.qp_num);
            else if (e.lane == Lane::Complete)
                live_qps.insert(polls[e.idx].payload.qp_num);
            else
                any_kernel = true;
        }

        on_rank(r.rank);
        // Kernels first, so compute reads at the top of a rank's lanes rather
        // than under a few hundred queue pairs — the same lane order, and the
        // same reason, as STREAMS in the Timeline panel.
        if (any_kernel)
            on_lane(r.rank, KERNEL_EP, "kernels");
        for (std::uint32_t qp : live_qps) {
            char nm[24];
            std::snprintf(nm, sizeof nm, "QP 0x%x", qp);
            on_lane(r.rank, qp, nm);
        }

        // The counter window, from this rank's own extent. Emitted through the
        // same callback stream as the slices and IN TIME ORDER with them, so an
        // encoder that needs monotonic timestamps gets them without having to
        // merge two sequences of its own.
        // The window opens at the first SEND, not at the first event. Anchoring
        // it to the first event put the first boundary a millisecond after a
        // kernel that ran ten milliseconds before any traffic, so the first
        // sample was emitted at a timestamp already behind the send that
        // triggered it — one backwards packet per rank, every time.
        const std::uint64_t span_ns = evs.empty() ? 0 : evs.back().t_ns - evs.front().t_ns;
        const std::uint64_t window =
            std::max(COUNTER_MIN_WINDOW_NS, span_ns / COUNTER_TARGET_SAMPLES);
        std::uint64_t win_end = 0;
        bool          have_window = false;
        std::uint64_t cum_bytes = 0;  // whole capture, for the cumulative track
        std::uint64_t win_bytes = 0;  // this window only, for the rate track

        auto flush_window = [&](std::uint64_t at) {
            on_counter(r.rank, Counter::BytesSent, at, static_cast<std::int64_t>(cum_bytes));
            // Bytes per window -> MB/s. Integer megabytes per second: the track
            // is read for shape and magnitude, and a double here would buy
            // precision the sampling has already spent.
            const auto rate = static_cast<std::int64_t>(win_bytes * 1000000000ull /
                                                        (window ? window : 1) / 1000000ull);
            on_counter(r.rank, Counter::SendRate, at, rate);
            st.counters += 2;
            win_bytes = 0;
        };

        for (const auto& e : evs) {
            // Close every window this event has passed, so a quiet stretch
            // reads as zero rather than as a gap the UI interpolates across.
            // Each boundary is emitted at win_end, and win_end is always past
            // the previous event by construction, so these stay in order with
            // the slices around them.
            while (have_window && e.t_ns >= win_end) {
                flush_window(win_end);
                win_end += window;
            }
            Slice s;
            s.t_ns = e.t_ns;
            s.dur_ns = e.dur_ns;
            s.rank = r.rank;
            s.lane = e.lane;
            switch (e.lane) {
                case Lane::Send: {
                    const auto& p = sends[e.idx].payload;
                    s.ep = p.qp_num;
                    // The name is the OPERATION, with the size in an annotation
                    // rather than in the text. Encoding it into the name ("send
                    // 4096B") splits Perfetto's own `select name, sum(dur) group by
                    // name` into one row per message size, which turns the first
                    // question anyone asks of a trace into nonsense.
                    s.name = "send";
                    s.wr_id = p.wr_id;
                    s.remote_addr = p.remote_addr;
                    s.bytes = p.length;
                    s.rkey = p.rkey;
                    s.opcode = p.opcode;
                    if (pairable.count(p.qp_num)) {
                        s.flow = next_flow++;
                        pending[{p.qp_num, p.wr_id}].push_back(s.flow);
                    }
                    cum_bytes += p.length;
                    win_bytes += p.length;
                    if (!have_window) {
                        have_window = true;
                        win_end = e.t_ns + window;
                    }
                    break;
                }
                case Lane::Complete: {
                    const auto& p = polls[e.idx].payload;
                    s.ep = p.qp_num;
                    s.name = "complete";
                    s.wr_id = p.wr_id;
                    s.bytes = p.byte_len;
                    s.opcode = p.opcode;
                    s.status = p.status;
                    s.flow_end = true;
                    if (!(p.opcode & WC_RECV_BIT) && pairable.count(p.qp_num)) {
                        auto it = pending.find({p.qp_num, p.wr_id});
                        if (it != pending.end() && !it->second.empty()) {
                            s.flow = it->second.front();
                            it->second.pop_front();
                            if (it->second.empty())
                                pending.erase(it);
                            st.flows++;
                        } else {
                            st.unmatched++;
                        }
                    }
                    break;
                }
                case Lane::Kernel: {
                    const auto& p = kernels[e.idx].payload;
                    s.ep = KERNEL_EP;
                    // Name it from the KernelName domain explicitly: interned
                    // handles restart at 1 in EVERY domain, so the defaulted
                    // one-argument str() resolves a valid handle to "" with no
                    // error. Table<GpuKernel> carries the same note.
                    const char* kn = r.str(rec::InternDomain::KernelName, p.name);
                    s.name = (kn && *kn) ? kn : "kernel";
                    break;
                }
            }
            on_slice(s);
            st.slices++;
        }
        // The last partial window. Without it the cumulative track stops short
        // of the total this rank actually sent, which is the one number on it
        // anybody reads off the right-hand edge.
        if (have_window)
            flush_window(evs.back().t_ns);
        // Sends still outstanding at the end of a rank belong to that rank, not
        // to the next one: (qp_num, wr_id) is only unique within a process.
        pending.clear();
    }
    return st;
}

// What both encoders say when a capture could not be placed on a wall clock, or
// when a completion named no send. Shared so the two cannot report a different
// story about the same capture.
inline void report_trace_stats(const TraceStats& st) {
    if (st.unanchored)
        std::fprintf(stderr,
                     "pcomm: %zu events had no clock anchor for their domain and are placed at "
                     "their raw tick — the capture is missing a ClockAnchor row\n",
                     st.unanchored);
    // Unmatched send-side completions are a finding about the capture, not a
    // rounding error: every one is a completion whose (qp_num, wr_id) named no
    // post_send this capture recorded. A handful means a trace that starts
    // mid-flight; a lot means the send path was not instrumented.
    if (st.unmatched)
        std::fprintf(stderr,
                     "pcomm: %zu send-side completions matched no post_send by (qp_num, wr_id) "
                     "and carry no flow\n",
                     st.unmatched);
    // Not a warning about the tool — a statement about the capture, and the
    // reason a lane is missing rather than wrong.
    if (st.unsignalled_qps)
        std::fprintf(stderr,
                     "pcomm: %zu queue pairs post more work requests than they complete "
                     "(%zu sends) — unsignalled WRs, so no completion can be attributed to a "
                     "specific send and these carry no flow\n",
                     st.unsignalled_qps, st.unsignalled_sends);
}

// A JSON string body. Kernel names come from the capture's intern dictionary,
// which is to say from whatever the application named its kernels — a demangled
// C++ name is JSON-safe by luck rather than by rule, and one stray quote turns
// the whole trace into a parse error at load time rather than a bad label.
inline std::string json_escape(const char* s) {
    std::string out;
    for (const char* p = s; p && *p; ++p) {
        switch (*p) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(*p) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof esc, "\\u%04x", *p);
                    out += esc;
                } else {
                    out += *p;
                }
        }
    }
    return out;
}

// --- Perfetto / Chrome JSON trace --------------------------------------------
// One process per rank, one thread per lane; every slice the model produces,
// with its annotations. Load in https://ui.perfetto.dev.
//
// The JSON encoding has no flow ids here on purpose: Chrome's flow events are a
// separate ph:"s"/"f" record pair rather than a field on the slice, and the
// pftrace is the format that gets loaded when someone wants the arrows. What
// the two DO share is every slice, every lane and every annotation.
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
    // Chrome JSON has no track hierarchy: a lane is a thread of a process, so
    // the rank is the pid and the endpoint is the tid. KERNEL_EP is 2^32-1,
    // which is a valid tid and cannot collide with a queue pair.
    auto lane_args = [&](const Slice& s) {
        // Every field the model carries, on every slice, unconditionally. The
        // JSON used to print a different arg set per lane and the pftrace
        // printed none, which is how a reader ended up having to know which
        // exporter they were looking at.
        std::fprintf(f,
                     ",\"args\":{\"wr\":%llu,\"bytes\":%u,\"opcode\":%u,\"status\":%u,"
                     "\"rkey\":%u,\"remote_addr\":\"0x%llx\"}}",
                     (unsigned long long)s.wr_id, s.bytes, s.opcode, s.status, s.rkey,
                     (unsigned long long)s.remote_addr);
    };

    const auto st = build_trace(
        ranks, clk, origin,
        [&](int rank) {
            sep();
            std::fprintf(f,
                         "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":%d,"
                         "\"args\":{\"name\":\"rank %d\"}}",
                         rank, rank);
        },
        [&](int rank, std::uint32_t ep, const char* name) {
            sep();
            std::fprintf(f,
                         "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":%d,\"tid\":%u,"
                         "\"args\":{\"name\":\"%s\"}}",
                         rank, ep, name);
        },
        [&](const Slice& s) {
            sep();
            const double ts = static_cast<double>(s.t_ns) / 1000.0;  // ns -> µs
            if (s.dur_ns)
                std::fprintf(f,
                             "{\"ph\":\"X\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f,"
                             "\"name\":\"%s\"",
                             s.rank, s.ep, ts, static_cast<double>(s.dur_ns) / 1000.0,
                             json_escape(s.name).c_str());
            else
                std::fprintf(f,
                             "{\"ph\":\"i\",\"s\":\"t\",\"pid\":%d,\"tid\":%u,\"ts\":%.3f,"
                             "\"name\":\"%s\"",
                             s.rank, s.ep, ts, json_escape(s.name).c_str());
            lane_args(s);
        },
        [&](int rank, Counter kind, std::uint64_t t_ns, std::int64_t value) {
            sep();
            // Chrome JSON counters are ph:"C" with the series in args. One
            // counter name per rank; Perfetto stacks them under the process.
            std::fprintf(f,
                         "{\"ph\":\"C\",\"pid\":%d,\"ts\":%.3f,\"name\":\"%s\","
                         "\"args\":{\"%s\":%lld}}",
                         rank, static_cast<double>(t_ns) / 1000.0,
                         kind == Counter::BytesSent ? "bytes sent" : "send MB/s",
                         kind == Counter::BytesSent ? "bytes" : "MB/s", (long long)value);
        });

    std::fputs("\n]}\n", f);
    std::fclose(f);
    std::printf("pcomm: %zu ranks, %zu events -> %s (load in ui.perfetto.dev)\n", ranks.size(),
                st.slices, outfile.c_str());
    report_trace_stats(st);
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
    // Fixed-width 64-bit (wire type 1), for the two fields that demand it:
    // TrackEvent.flow_ids (47) and .terminating_flow_ids (48) are declared
    // `repeated fixed64`. Their deprecated twins at 36 and 42 are the varint
    // ones. Encoding a flow id as a varint under field 47 yields a packet
    // Perfetto ingests without complaint and draws no arrow for, which is a
    // silent failure worth one method to avoid.
    void f64(int field, std::uint64_t v) {
        tag(field, 1);
        for (int i = 0; i < 8; ++i)
            b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
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

    // Sequence 1 carries the track descriptors; every rank gets its own
    // sequence for events. Perfetto asks that packets be timestamp-ordered
    // WITHIN a sequence, not across the trace, so a sequence per rank is what
    // lets 64 ranks interleave without any of them going backwards. The
    // per-rank sort below is what makes each one actually monotonic.
    constexpr std::uint64_t SEQ_DESC = 1;
    auto rank_seq = [](std::size_t i) { return static_cast<std::uint64_t>(i) + 2; };
    auto rank_uuid = [](int rank) { return static_cast<std::uint64_t>(rank) + 1; };
    auto qp_uuid = [](int rank, std::uint32_t qp) {
        return (static_cast<std::uint64_t>(rank) + 1) * 0x100000000ull + qp;
    };
    // Kernels get their own track per rank, sharing the QP tracks' parent so
    // compute reads directly against comm — the same placement, and the same
    // reason, as the timeline panel's lane order. The uuid space is disjoint
    // from qp_uuid's by construction: a QP number is 32 bits, this sets bit 63.
    auto kernel_uuid = [](int rank) {
        return 0x8000000000000000ull + static_cast<std::uint64_t>(rank) + 1;
    };
    // Counter tracks get a third disjoint uuid space: bit 62, two per rank.
    // qp_uuid occupies (rank+1)<<32 | qp and kernel_uuid sets bit 63, so none
    // of the three can collide however many ranks or queue pairs a run has.
    auto counter_uuid = [](int rank, Counter k) {
        return 0x4000000000000000ull + (static_cast<std::uint64_t>(rank) + 1) * 2 +
               static_cast<std::uint64_t>(k);
    };
    PB   trace;
    auto track = [&](std::uint64_t uuid, const std::string& name, std::uint64_t parent) {
        PB td;
        td.u64(1, uuid);
        td.str(2, name);
        if (parent)
            td.u64(5, parent);
        PB pkt;
        pkt.msg(60, td);
        pkt.u64(10, SEQ_DESC);
        trace.msg(1, pkt);
    };
    // A counter track. TrackDescriptor.counter is field 8; inside
    // CounterDescriptor, unit is 3 (UNIT_SIZE_BYTES = 3), unit_name is 6 and
    // is_incremental is 5. is_incremental stays FALSE: the samples below carry
    // absolute values, and telling Perfetto they are deltas would make the
    // cumulative track climb as the square of the traffic.
    auto counter_track = [&](std::uint64_t uuid, const std::string& name, std::uint64_t parent,
                             bool bytes) {
        PB cd;
        if (bytes)
            cd.u64(3, 3);  // unit = UNIT_SIZE_BYTES
        else
            cd.str(6, "MB/s");  // unit_name
        cd.u64(5, 0);           // is_incremental = false
        PB td;
        td.u64(1, uuid);
        td.str(2, name);
        td.u64(5, parent);
        td.msg(8, cd);
        PB pkt;
        pkt.msg(60, td);
        pkt.u64(10, SEQ_DESC);
        trace.msg(1, pkt);
    };
    // One annotation. DebugAnnotation.name is field 10 (string) and the value
    // fields are a oneof: uint_value 3, pointer_value 7. An address goes in as
    // a pointer so Perfetto renders it as hex rather than as a 15-digit
    // decimal nobody can match against an mr.
    auto annot = [](PB& ev, const char* key, std::uint64_t v, bool pointer = false) {
        PB da;
        da.str(10, key);
        da.u64(pointer ? 7 : 3, v);
        ev.msg(4, da);  // TrackEvent.debug_annotations
    };
    // BEGIN and END are emitted as separate packets from a single ordered list
    // rather than as a pair, because they are ordered independently: a slice
    // lasting 17 ms ends long after the next slice begins, and emitting its END
    // straight after its BEGIN walks the sequence backwards. Perfetto asks for
    // monotonic timestamps within a sequence, and a pair cannot give that.
    //
    // The flow ids and the annotations both ride the BEGIN, because that is the
    // packet Perfetto binds to the slice.
    auto begin_pkt = [&](std::uint64_t seq, std::uint64_t track_uuid, const Slice& s) {
        PB ev;
        ev.u64(9, s.dur_ns ? 1u : 3u);  // SLICE_BEGIN | INSTANT
        ev.u64(11, track_uuid);
        ev.str(23, s.name);
        if (s.flow)
            ev.f64(s.flow_end ? 48 : 47, s.flow);
        // The same fields the JSON encoder writes, from the same model. A
        // pftrace slice used to carry nothing but a name, so clicking one in
        // Perfetto answered no question at all.
        if (s.lane != Lane::Kernel) {
            annot(ev, "wr_id", s.wr_id);
            annot(ev, "bytes", s.bytes);
            annot(ev, "opcode", s.opcode);
        }
        if (s.lane == Lane::Send) {
            if (s.remote_addr)
                annot(ev, "remote_addr", s.remote_addr, /*pointer=*/true);
            if (s.rkey)
                annot(ev, "rkey", s.rkey);
        }
        if (s.lane == Lane::Complete)
            annot(ev, "status", s.status);
        PB pkt;
        pkt.u64(8, s.t_ns);
        pkt.msg(11, ev);
        pkt.u64(10, seq);
        trace.msg(1, pkt);
    };
    auto end_pkt = [&](std::uint64_t seq, std::uint64_t track_uuid, std::uint64_t t_ns) {
        PB ev;
        ev.u64(9, 2u);  // SLICE_END
        ev.u64(11, track_uuid);
        PB pkt;
        pkt.u64(8, t_ns);
        pkt.msg(11, ev);
        pkt.u64(10, seq);
        trace.msg(1, pkt);
    };

    // Perfetto wants each sequence monotonic, and build_trace hands slices over
    // in start order — but a slice's END is ordered independently of the next
    // slice's BEGIN, so the ENDs are buffered per rank and merged back in. The
    // heap holds only what is still open, which on this corpus is single
    // digits: the alternative, materialising both endpoints of every slice and
    // sorting, costs an extra pass over 23 M packets to answer the same
    // question.
    struct Open {
        std::uint64_t end_ns;
        std::uint64_t track_uuid;
    };
    auto              later = [](const Open& a, const Open& b) { return a.end_ns > b.end_ns; };
    std::vector<Open> open;
    std::uint64_t     seq = 0;
    int               cur_rank = -1;
    std::size_t       rank_ix = 0;

    // Everything still open when a rank ends, in order. A rank's sequence must
    // not carry a packet after the next rank starts writing to its own.
    auto drain = [&](std::uint64_t until) {
        while (!open.empty() && open.front().end_ns <= until) {
            std::pop_heap(open.begin(), open.end(), later);
            end_pkt(seq, open.back().track_uuid, open.back().end_ns);
            open.pop_back();
        }
    };

    const auto st = build_trace(
        ranks, clk, origin,
        [&](int rank) {
            drain(~0ull);  // close out the previous rank before switching sequence
            open.clear();
            cur_rank = rank;
            seq = rank_seq(rank_ix++);
            track(rank_uuid(rank), "rank " + std::to_string(rank), 0);
            counter_track(counter_uuid(rank, Counter::BytesSent), "bytes sent", rank_uuid(rank),
                          /*bytes=*/true);
            counter_track(counter_uuid(rank, Counter::SendRate), "send MB/s", rank_uuid(rank),
                          /*bytes=*/false);
        },
        [&](int rank, std::uint32_t ep, const char* name) {
            track(ep == KERNEL_EP ? kernel_uuid(rank) : qp_uuid(rank, ep), name, rank_uuid(rank));
        },
        [&](const Slice& s) {
            const auto uuid = s.ep == KERNEL_EP ? kernel_uuid(s.rank) : qp_uuid(s.rank, s.ep);
            drain(s.t_ns);
            begin_pkt(seq, uuid, s);
            if (s.dur_ns) {
                open.push_back({s.t_ns + s.dur_ns, uuid});
                std::push_heap(open.begin(), open.end(), later);
            }
        },
        [&](int rank, Counter kind, std::uint64_t t_ns, std::int64_t value) {
            // Drain first: a counter sample lands between slices, and any END
            // still open from before it must be written before it or the
            // sequence walks backwards.
            drain(t_ns);
            PB ev;
            ev.u64(9, 4u);  // TYPE_COUNTER
            ev.u64(11, counter_uuid(rank, kind));
            ev.u64(30, static_cast<std::uint64_t>(value));  // counter_value
            PB pkt;
            pkt.u64(8, t_ns);
            pkt.msg(11, ev);
            pkt.u64(10, seq);
            trace.msg(1, pkt);
        });
    drain(~0ull);
    (void)cur_rank;

    std::FILE* f = std::fopen(outfile.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "pcomm: cannot write %s\n", outfile.c_str());
        return 1;
    }
    std::fwrite(trace.b.data(), 1, trace.b.size(), f);
    std::fclose(f);
    std::printf("pcomm: %zu ranks, %zu events, %zu send->completion flows -> %s "
                "(%zu bytes; load in ui.perfetto.dev)\n",
                ranks.size(), st.slices, st.flows, outfile.c_str(), trace.b.size());
    report_trace_stats(st);
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
