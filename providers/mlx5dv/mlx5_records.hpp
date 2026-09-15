#pragma once
// pcomm mlx5dv/DEVX records (id range 200-299, UNIFICATION §3.4).
//
// This range exists for the traffic the ibverbs provider structurally cannot
// see. IBGDA — NVSHMEM's GPU-initiated transport — never calls ibv_create_qp,
// never calls ibv_modify_qp, and never calls ibv_post_send: it drives DEVX
// directly and the GPU rings the NIC doorbell itself. Captured through the
// 100-range alone, such a run yields no queue pairs, no topology and no
// operations, and yields them *silently*, as empty tables.
//
// Vendor-neutral like ib_records.hpp: the provider decodes the mlx5 command
// buffers and emits these, so nothing here includes mlx5dv.h or mlx5_ifc.h.

#include <cstdint>

#include "pcomm/core/ids.hpp"
#include "pcomm/core/record.hpp"
#include "pcomm/core/vocab.hpp"

namespace pcomm::providers::mlx5dv::records {

using core::DeviceId;
using core::Handle;
using core::MemRange;
using core::RecordKind;

// A device ordinal we could not determine. Declared in core/vocab.hpp beside
// DeviceId since ibverbs answers the same question; re-exported here so the
// records below still read as they did.
using core::kUnknownDevice;

// How a buffer was handed to the NIC. This is not bookkeeping: it decides
// whether the buffer's *address* means anything.
//
//   ByAddress — classic registration, addr is a real VA, so the owning device
//               can be resolved by asking CUDA about the pointer.
//   ByDmabuf  — registered by dmabuf file descriptor, addr is an offset (always
//               observed as 0) and there is no pointer to classify.
//
// Measured on du04: under NVSHMEM_IB_DISABLE_DMABUF=1 (peermem path)
// registrations arrive ByAddress; with DMABUF enabled every one arrives
// ByDmabuf with comp_mask=0x1 and addr=0x0. SigmaTrace's GPU-residency test
// reads the address and therefore only works in the first configuration —
// supporting both is where this provider is meant to exceed the prior art.
enum class UmemBinding : std::uint8_t { ByAddress = 0, ByDmabuf = 1 };

// Which exported symbol the registration came through. Recorded rather than
// assumed: NVSHMEM 3.4.5's IBGDA calls *only* mlx5dv_devx_umem_reg_ex
// (measured reg=0, reg_ex=10), while rdma-core may implement the legacy entry
// over the _ex one — which is exactly how hooking both ibv_reg_mr and
// ibv_reg_mr_iova2 double-counted every region in case 01. With the entry point
// on the row, double counting is visible in the capture instead of inferred.
enum class UmemEntry : std::uint8_t { Reg = 0, RegEx = 1 };

// ENTITY — a buffer handed to the NIC via DEVX, lifetime [reg, dereg).
PCOMM_RECORD(Mlx5Umem, RecordKind::Entity, 200, "mlx5_umem") {
    Handle        umem;     // interned mlx5dv_devx_umem*
    MemRange      range;    // addr + size; addr is 0 for a dmabuf binding
    std::uint32_t umem_id;  // what the DEVX command buffers refer to it by
    std::uint32_t access;
    std::int32_t  dmabuf_fd;  // -1 unless binding == ByDmabuf
    DeviceId      device;     // owning GPU, or kUnknownDevice
    std::uint8_t  binding;    // UmemBinding
    std::uint8_t  entry;      // UmemEntry
};

// ENTITY — a User Access Region: the doorbell page. The GPU writes 8 bytes here
// to tell the NIC a work-queue entry is ready, which is the entire data plane
// of a GPU-initiated send.
PCOMM_RECORD(Mlx5Uar, RecordKind::Entity, 202, "mlx5_uar") {
    Handle        uar;       // interned mlx5dv_devx_uar*
    std::uint64_t reg_addr;  // the doorbell register itself
    std::uint64_t base_addr;
    std::uint32_t page_id;
    std::uint32_t flags;
};

// EVENT — a DEVX command we saw but did not decode.
//
// Deliberately recorded rather than dropped. An unrecognised opcode is the
// version boundary this provider is most likely to hit, and the standing lesson
// from find_nvshmem_heap_owner is that introspection into another project's
// internals must fail *loudly* there. A capture that quietly omits the command
// that created the queue pairs is indistinguishable from a run that made none.
PCOMM_RECORD(Mlx5DevxCmd, RecordKind::Event, 204, "mlx5_devx_cmd") {
    Handle        obj;      // interned mlx5dv_devx_obj*, 0 if the call failed
    std::uint32_t opcode;   // MLX5_CMD_OP_*
    std::uint8_t  decoded;  // 1 if a typed record was also emitted for this
};

// ENTITY — a DEVX queue pair: the GPU-initiated equivalent of IbQueuePair.
//
// sq_base + log_sq_size locate the send queue, which is where the GPU writes
// work-queue entries. When the WQ buffer is GPU-resident this is a device-driven
// QP and its doorbells come from the GPU; when it is not, the queue pair is
// CPU-side and cannot produce one.
PCOMM_RECORD(Mlx5DevxQp, RecordKind::Entity, 206, "mlx5_devx_qp") {
    Handle        obj;      // interned mlx5dv_devx_obj*
    std::uint64_t sq_base;  // WQ umem address + wq_umem_offset
    std::uint32_t qp_num;
    std::uint32_t wq_umem_id;    // joins to Mlx5Umem.umem_id
    std::uint32_t log_sq_size;   // WQEs = 1 << log_sq_size
    DeviceId      device;        // owning GPU of the WQ buffer, or kUnknownDevice
    // The send completion queue this queue pair reports into, from the create
    // command's qpc.cqn_snd. It is the join that makes a completion attributable
    // to a queue pair, and it was free all along — the field has been in every
    // CREATE_QP command this provider already decodes.
    std::uint32_t cqn_snd;
    std::uint8_t  service_type;  // mlx5 transport service type (0x5 = DCI)
    std::uint8_t  gpu_driven;    // 1 if the WQ buffer is GPU-resident
};

// ENTITY — a DEVX completion queue: where the NIC reports what it finished.
//
// The other half of the data plane, and the half pcomm has never had. Under
// IBGDA the CQ is GPU-resident and polled by GPU threads with ordinary loads,
// so a completion is consumed with no host call, no MMIO, and no NIC-visible
// artifact — `ibgda_poll_cq` ends in an atomicMax on a software consumer index
// and never writes a CQ doorbell record. There is nothing to interpose, and
// this record does not pretend otherwise: it captures the CQ's IDENTITY and
// GEOMETRY, not its polls.
//
// That is worth having on its own. It gives mlx5_doorbell and mlx5_device_send
// a completion queue to join to through Mlx5DevxQp.cqn_snd; it says how many
// CQs a PE holds, which is the input to the one cost law the mechanism has —
// a completion that must cover every queue pair of a PE pays per queue pair;
// and cq_base locates the ring for anything that later reads it in place.
PCOMM_RECORD(Mlx5DevxCq, RecordKind::Entity, 216, "mlx5_devx_cq") {
    Handle        obj;      // interned mlx5dv_devx_obj*
    std::uint64_t cq_base;  // CQ umem address + cq_umem_offset
    std::uint32_t cqn;      // what qpc.cqn_snd refers to it by
    std::uint32_t umem_id;  // joins to Mlx5Umem.umem_id
    std::uint32_t log_cq_size;   // CQEs = 1 << log_cq_size
    std::uint32_t dbr_umem_id;   // the CQ's doorbell-record buffer
    DeviceId      device;        // owning GPU of the CQ buffer, or kUnknownDevice
    std::uint8_t  gpu_resident;  // 1 if the CQ buffer is GPU-resident
    std::uint8_t  cqe_size;      // cqc.cqe_sz (0 = 64 B, 1 = 128 B)
};

// EVENT — the ->RTR transition of a DEVX queue pair, carrying the remote
// endpoint. This is the IBGDA equivalent of IbModifyQp's RTR row, and it is
// what makes the connection topology recoverable.
//
// src_lid is here from the outset rather than added after the fact: qp_num is
// allocated per HCA, so two ranks on different nodes routinely draw the same
// one, and a reader keying peers on qp_num alone attributes edges to whichever
// rank it scanned last — including impossible self-edges. That cost case 02 a
// wrong topology and a record-type bump to fix (758e975). Same collision here,
// no reason to relearn it.
PCOMM_RECORD(Mlx5DevxQpConnect, RecordKind::Event, 208, "mlx5_devx_qp_connect") {
    std::uint64_t dest_gid_prefix;  // remote GID, high 8 bytes
    std::uint64_t dest_gid_iface;   // remote GID, low 8 bytes
    std::uint32_t qp_num;
    std::uint32_t dest_qp_num;
    std::uint32_t dest_lid;
    std::uint32_t src_lid;
    std::uint8_t  port_num;
};

// ENTITY — a queue pair created through mlx5dv_create_qp, lifetime [create,
// destroy). Not a DEVX object: this is an ordinary verbs queue pair reached
// through the mlx5 extension entry point, which is why it carries an ibv_qp
// handle and a qp_type rather than a send-queue address.
//
// It exists because perftest creates its queue pairs this way and never calls
// ibv_create_qp, so ib_queue_pair has been empty on case 08 for as long as the
// case has existed — while three goldens sat green beside it and its 3
// modify_qp transitions referred to a queue pair no record had created. The
// entity is emitted here rather than as an IbQueuePair because a provider may
// only write inside its declared id range, which the engine now enforces.
PCOMM_RECORD(Mlx5QueuePair, RecordKind::Entity, 210, "mlx5_queue_pair") {
    Handle        qp;  // interned ibv_qp*
    Handle        pd;  // interned ibv_pd*
    std::uint32_t qp_num;
    std::uint8_t  qp_type;
};

// EVENT — the GPU rang a doorbell. The data plane of a GPU-initiated send made
// visible: the doorbell hijack redirects the UAR page, decodes the 8-byte store
// the GPU wrote, and emits this. `qp_num` and `prod_idx` come straight from the
// mlx5 control segment (big-endian `qpn_ds`/`opmod_idx_opcode`).
//
// Emitted even when the WQE behind it cannot be read, so "the doorbell rang and
// we could not decode it" is a visible row and not silence — the distinction the
// whole doorbell-hijack line of work exists to keep. `uar` joins Mlx5Uar; every
// `qp_num` should join a Mlx5DevxQp some rank created.
PCOMM_RECORD(Mlx5Doorbell, RecordKind::Event, 212, "mlx5_doorbell") {
    Handle        uar;  // interned mlx5dv_devx_uar* (joins Mlx5Uar)
    std::uint32_t qp_num;
    std::uint32_t prod_idx;  // producer index the doorbell announced
};

// How much of the WQE the decode read. On the row because "every field was
// read" and "this build does not know this opcode's layout" must not look the
// same in the capture — the rule Mlx5DevxCmd.decoded already applies one layer
// up. See `op_shape` in uar_hijack.hpp for what each opcode carries.
enum class SendDecode : std::uint8_t {
    Full = 0,       // every segment the opcode names was located and read
    Partial = 1,    // remote address read; local descriptor past unmodelled
                    // atomic operand segments, so laddr/lkey/length are 0
    Truncated = 2,  // ds did not cover the opcode's own segments
    UnknownOp = 3,  // opcode not in the layout table; only ids are meaningful
};

// Which door saw the WQE a Mlx5DeviceSend row was decoded from — and so what
// the row's time means. Mirrored as hijack::kSubmitter* in uar_hijack.hpp (which
// cannot include this header); the provider static_asserts the two equal.
enum class SendSubmitter : std::uint8_t {
    Unknown = 0,       // never written on purpose: a call site that forgot to say
    GpuDoorbell = 1,   // uar_hijack.hpp — the GPU's doorbell store (IBGDA). Instant
    HostDoorbell = 2,  // host_doorbell.hpp — a host thread's doorbell (CPU-assisted IBGDA). Instant
    SqWatch = 3,       // sq_watch.hpp — the doorbell record moving (ibdevx). Window
};

// EVENT — a send work-queue entry, decoded. The WQE-level counterpart of
// IbPostSend, and deliberately the same shape (opcode, local, remote, length) so
// a reader can union the two: when a transport writes its WQEs itself — IBGDA
// from the GPU, ibdevx from a CPU proxy — ib_post_send is empty and this is the
// only send record there is.
//
// The name is older than its meaning. It was written for the GPU doorbell, when
// "device send" was accurate; the host doorbell and the send-queue watcher now
// decode through the same parser, and `submitter` says which door saw each row.
// The wire name stays: renaming it would touch every reader of the table for no
// gain in meaning, once the row itself says where it came from.
//
// What the row's TIME means depends on `submitter`, and the time kind says so.
// The doorbell doors record an Instant — the doorbell itself, intercepted before
// the NIC sees it. The watcher records a Window: the WQE was published after its
// previous look at this queue and before this one, each look bracketed by TSC
// reads either side of the doorbell-record read, so the bound is exact and
// typically a few microseconds wide. It is never the post itself.
PCOMM_RECORD(Mlx5DeviceSend, RecordKind::Event, 214, "mlx5_device_send") {
    std::uint64_t laddr;  // local address (data segment)
    std::uint64_t raddr;  // remote address (RDMA segment), 0 if none
    std::uint32_t qp_num;
    std::uint32_t wqe_idx;  // producer index of this WQE
    std::uint32_t lkey;
    std::uint32_t rkey;    // remote key, 0 if no RDMA segment
    std::uint32_t length;  // bytes this WQE moves
    // The destination DCT of a DC send, from the low 24 bits of the address
    // vector's dqp_dct. 0 on RC, where the peer is a property of the queue pair
    // and Mlx5DevxQpConnect already carries it. Without this a DC capture
    // records traffic with no peer at all, which is most of what a
    // communication profiler is for — NVSHMEM shipped DC-only through its 2.x
    // line and still supports it.
    std::uint32_t dest_dct;
    std::uint8_t  opcode;  // mlx5 WQE opcode (0x08 RDMA_WRITE, 0x0a SEND, …)
    std::uint8_t  decode;  // SendDecode
    // Width of the address vector this WQE actually carried, in 16-byte
    // descriptor units: 0 on RC (no AV), 1 for the compact 16-byte form, 3 for
    // the full 48-byte form. DERIVED per WQE from MLX5_EXTENDED_UD_AV — and
    // RECORDED because deriving it correctly and then not saying which was
    // derived is what left the full-AV branch shipped and unexecuted. A run
    // whose dc_full leg silently produced compact AVs was indistinguishable
    // from one that never ran (MN5 job 45452104); with this on the row it is
    // one query, on any capture, forever.
    std::uint8_t av_ds;
    std::uint8_t submitter;  // SendSubmitter: which door saw it, so what its time means
};

// ARTIFACT — how much of the data plane one door saw, per rank: the send-queue
// watcher's counters, which until now reached stderr and nowhere a query could.
//
// The identity it exists to expose: every WQEBB the producers published past a
// watched queue's starting point is exactly one of decoded, lapped or dropped,
//
//     published == decoded + lapped + dropped
//
// so an assertion can demand `lapped + dropped == 0` and know it asked the whole
// question. (du04, case 11's workload at 1024 x 20: published = decoded =
// 20,543 on rank 0.)
//
// Emitted once per rank from the watcher's own atexit handler. atexit is LIFO
// and the watcher starts after the tracer, so that handler runs BEFORE the
// tracer's finalize — the provider's destructor, where the counters used to be
// printed, runs after it and could not have recorded anything. Like
// CaptureStatus, its ABSENCE therefore means something: a rank with watched
// queues and no row did not reach a normal exit, and its coverage is unknown.
//
// Only the send-queue watcher writes it. The doorbell doors keep their counters
// on stderr: they do not count what was published in WQEBBs, so a row from them
// could not honour the identity above, and a row that cannot is worse than none.
PCOMM_RECORD(Mlx5SendCoverage, RecordKind::Artifact, 218, "mlx5_send_coverage") {
    std::uint64_t published;        // WQEBBs published past the watched queues' start
    std::uint64_t decoded;          // ...parsed into Mlx5DeviceSend rows
    std::uint64_t lapped;           // ...overwritten before they could be read
    std::uint64_t dropped;          // ...read, but the staging ring was full
    std::uint64_t rows;             // Mlx5DeviceSend rows this door emitted (NOPs make none)
    std::uint64_t rows_unknown_op;  // ...of which the opcode had no layout
    std::uint64_t rows_truncated;   // ...of which ds did not cover the segments
    std::uint32_t queues;           // queue pairs watched
    std::uint32_t refused;          // queue pairs declined (the reason is on stderr)
    std::uint32_t held;             // looks that waited out an uninitialised doorbell record
    std::uint8_t  submitter;        // SendSubmitter — SqWatch, today always
    std::uint8_t  pad0;
    std::uint16_t pad1;
};

}  // namespace pcomm::providers::mlx5dv::records
