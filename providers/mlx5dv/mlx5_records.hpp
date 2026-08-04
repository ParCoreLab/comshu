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

// A device ordinal we could not determine. Not 0 — 0 is a real GPU, and the
// whole failure family this tree keeps hitting is a plausible value standing in
// for a missing one.
inline constexpr DeviceId kUnknownDevice = 0xFFFF;

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
    Handle        umem;      // interned mlx5dv_devx_umem*
    MemRange      range;     // addr + size; addr is 0 for a dmabuf binding
    std::uint32_t umem_id;   // what the DEVX command buffers refer to it by
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
    Handle        obj;     // interned mlx5dv_devx_obj*, 0 if the call failed
    std::uint32_t opcode;  // MLX5_CMD_OP_*
    std::uint8_t  decoded;  // 1 if a typed record was also emitted for this
};

// ENTITY — a DEVX queue pair: the GPU-initiated equivalent of IbQueuePair.
//
// sq_base + log_sq_size locate the send queue, which is where the GPU writes
// work-queue entries. When the WQ buffer is GPU-resident this is a device-driven
// QP and its doorbells come from the GPU; when it is not, the queue pair is
// CPU-side and cannot produce one.
PCOMM_RECORD(Mlx5DevxQp, RecordKind::Entity, 206, "mlx5_devx_qp") {
    Handle        obj;          // interned mlx5dv_devx_obj*
    std::uint64_t sq_base;      // WQ umem address + wq_umem_offset
    std::uint32_t qp_num;
    std::uint32_t wq_umem_id;   // joins to Mlx5Umem.umem_id
    std::uint32_t log_sq_size;  // WQEs = 1 << log_sq_size
    DeviceId      device;       // owning GPU of the WQ buffer, or kUnknownDevice
    std::uint8_t  service_type;  // mlx5 transport service type (0x5 = DCI)
    std::uint8_t  gpu_driven;    // 1 if the WQ buffer is GPU-resident
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

}  // namespace pcomm::providers::mlx5dv::records
