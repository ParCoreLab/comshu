// mlx5dv/DEVX provider — the control plane the ibverbs provider cannot see.
//
// Ported from SigmaTrace/Profiler/Handlers/IBV/Mlx5dv.cpp, with the two
// bindings that its version predates rewritten rather than copied (see
// mlx5_records.hpp and mlx5_hooks.hpp for the measurements behind each):
//
//   * mlx5dv_devx_umem_reg_ex is hooked as well as the legacy entry point, and
//     each row records which one it came through.
//   * GPU residency is not decided by asking CUDA about the umem address,
//     because under dmabuf registration there is no address to ask about.
//
// One rule governs everything here, learned the hard way twice in one session.
// A probe of this same interface returned nullptr when it could not resolve a
// real symbol; NVSHMEM read that as a failed UAR allocation and disabled its
// entire transport. And SigmaTrace's `.at()` on an unregistered queue pair
// aborts the application on its first device-initiated send. So: a hook that
// meets something it does not recognise RECORDS that fact and gets out of the
// way. It never synthesises a failure, and it never terminates.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <endian.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

#include <mlx5/mlx5_ifc.h>
#include <mlx5/mlx5_prm.h>

#include "pcomm/core/ids.hpp"
#include "pcomm/core/mem_attr.hpp"
#include "pcomm/core/records.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/tracer.hpp"

#include "mlx5_event_source.hpp"
#include "pcomm/core/dmabuf_registry.hpp"
#include "mlx5_records.hpp"
#include "host_doorbell.hpp"
#include "sq_watch.hpp"
#include "uar_hijack.hpp"

namespace mlx5 {

namespace rr = pcomm::providers::mlx5dv::records;
using pcomm::rec::InternDomain;
using rr::kUnknownDevice;
using rr::UmemBinding;
using rr::UmemEntry;

static pcomm::rec::Recorder<>& rec() {
    static pcomm::rec::Recorder<> proxy;
    return proxy;
}

static pcomm::core::Handle h(const void* p) {
    return rec().intern(InternDomain::Pointer, reinterpret_cast<std::uint64_t>(p));
}

// --- optional CUDA ---------------------------------------------------------
//
// Device attribution is core::device_of (core/mem_attr.hpp), not a copy here.
// It moved out when ibverbs needed the same answer: the constants it depends on
// are hand-copied from cuda.h and unchecked by any compiler, and one of them
// was wrong for this provider's entire life. One copy, one place to be wrong.
//
// Resolved at runtime rather than linked, so this provider still builds and
// runs on a host with no CUDA at all (MN5's core configuration does exactly
// that) and reports kUnknownDevice there — a distinct value from "device 0" on
// purpose.

// --- umem registry ---------------------------------------------------------
//
// DEVX command buffers name buffers by umem_id, so decoding CREATE_QP into a
// send-queue address requires remembering what each id was. Kept here rather
// than reconstructed by the reader because the reader would have to join across
// streams and would still not know the owning device.
struct UmemInfo {
    std::uint64_t         addr{0};
    std::uint64_t         size{0};
    pcomm::core::DeviceId device{kUnknownDevice};
    UmemBinding           binding{UmemBinding::ByAddress};
    std::int32_t          dmabuf_fd{-1};
    // The buffer's real GPU virtual address base: the umem addr for a by_addr
    // binding, the dmabuf-exported range base for by_dmabuf. This is what the
    // doorbell hijack copies WQEs from — the recorded sq_base stays an offset
    // under dmabuf on purpose (umem_binding_as_configured asserts that), so the
    // GPU address is kept here separately rather than by rewriting the row.
    std::uint64_t gpu_base{0};
    // The umem object itself, so the send-queue watcher can be told a buffer is
    // going away by the dereg hook, which sees the pointer and not the id.
    const void* umem{nullptr};
};

static std::mutex                                  g_umem_mutex;
static std::unordered_map<std::uint32_t, UmemInfo> g_umems;
// DEVX objects carry no context pointer into obj_modify, so the context is
// remembered at create time. Without it there is no way to query our own port's
// LID, and without that a queue pair has no globally unique name.
static std::mutex                                    g_obj_mutex;
static std::unordered_map<const void*, ibv_context*> g_obj_ctx;

static void remember_umem(std::uint32_t id, const UmemInfo& info) {
    std::lock_guard<std::mutex> lk(g_umem_mutex);
    g_umems[id] = info;
}

static bool lookup_umem(std::uint32_t id, UmemInfo* out) {
    std::lock_guard<std::mutex> lk(g_umem_mutex);
    auto                        it = g_umems.find(id);
    if (it == g_umems.end())
        return false;
    *out = it->second;
    return true;
}

static void record_umem(mlx5dv_devx_umem* out, const void* addr, std::size_t size,
                        std::uint32_t access, std::int32_t dmabuf_fd, UmemEntry entry) {
    if (!out)
        return;
    const bool by_dmabuf = dmabuf_fd >= 0;

    // Under dmabuf, `addr` is an OFFSET into the descriptor and the Mlx5Umem row
    // keeps it that way. umem_binding_as_configured fails a by_dmabuf row whose
    // address is nonzero, precisely to catch an offset being reported as a
    // pointer, and that check stays load-bearing — so nothing here rewrites it.
    //
    // What the descriptor can NOW yield is the range it was exported from:
    // NVSHMEM called cuMemGetHandleForAddressRange(&fd, dptr, size, ...) one
    // step earlier and pcomm::core::dmabuf::lookup carries that here. It feeds two things
    // that were previously impossible, neither of which is the umem's address:
    //
    //   - the owning device, which gpu_driven otherwise has to infer from the
    //     binding alone (the heuristic that made case 09's gate circular);
    //   - a MemObject with a real range, so a dmabuf buffer becomes nameable.
    //
    // If the conversion was not observed, both stay unknown. That is the answer
    // this provider gave for every dmabuf umem until now, so a miss is a
    // regression to the previous behaviour rather than a new fiction.
    pcomm::core::dmabuf::Range dmabuf_range{};
    const bool resolved = by_dmabuf && pcomm::core::dmabuf::lookup(dmabuf_fd, &dmabuf_range);

    const std::uint64_t gpu_base =
        by_dmabuf ? (resolved ? dmabuf_range.base : 0) : reinterpret_cast<std::uint64_t>(addr);
    UmemInfo info{reinterpret_cast<std::uint64_t>(addr),
                  size,
                  by_dmabuf ? (resolved ? dmabuf_range.device : kUnknownDevice)
                            : pcomm::core::device_of(addr),
                  by_dmabuf ? UmemBinding::ByDmabuf : UmemBinding::ByAddress,
                  dmabuf_fd,
                  gpu_base,
                  out};
    remember_umem(out->umem_id, info);
    rec().record(pcomm::core::Temporal::none(),
                 rr::Mlx5Umem{{},
                              {},
                              h(out),
                              {info.addr, info.size},
                              out->umem_id,
                              access,
                              dmabuf_fd,
                              info.device,
                              static_cast<std::uint8_t>(info.binding),
                              static_cast<std::uint8_t>(entry)});

    // ...and the buffer itself, in the shared vocabulary.
    //
    // by_addr: the umem's own address is the buffer's address.
    //
    // by_dmabuf: only once the export was observed. The buffer's address is the
    // exported range's base PLUS the umem's offset — using the range base alone
    // would put every sub-range on top of the first, and case 09 carves three
    // queue pairs out of one 327680-byte buffer at offsets 0x0, 0x10000 and
    // 0x20000, so they would collapse into one object.
    //
    // Unresolved dmabuf still emits NOTHING. A MemObject with base 0 and this
    // size would be a fiction that range containment then resolves real
    // addresses against — worse than a missing row, and unrecoverable after the
    // fact.
    //
    // Interned on the resulting base, in the same domain ibverbs and gpumem use,
    // so a buffer seen by two providers mints one id rather than two rows nobody
    // can tell apart.
    if (!by_dmabuf) {
        rec().record(
            rec().now_instant(),
            pcomm::core::records::MemObject{
                {}, {}, h(addr), pcomm::core::kNullHandle, {info.addr, info.size}, info.device});
    } else if (resolved) {
        const std::uint64_t base = dmabuf_range.base + reinterpret_cast<std::uint64_t>(addr);
        rec().record(rec().now_instant(),
                     pcomm::core::records::MemObject{{},
                                                     {},
                                                     h(reinterpret_cast<const void*>(base)),
                                                     pcomm::core::kNullHandle,
                                                     {base, info.size},
                                                     info.device});
    }
}

// Our own port's LID, so (src_lid, qp_num) names this queue pair uniquely.
static std::uint32_t port_lid(ibv_context* ctx, std::uint8_t port_num) {
    if (!ctx || port_num == 0)
        return 0;
    ibv_port_attr pa{};
    return ibv_query_port(ctx, port_num, &pa) == 0 ? pa.lid : 0u;
}

// Per-QP source LID, learned at RST2INIT.
//
// A queue pair is bound to a port at the ->INIT transition and nowhere else, so
// that is the one command that can say which port — exactly as the ibverbs
// provider learns it under IBV_QP_PORT (ib_records.hpp). Reading
// primary_address_path.vhca_port_num out of INIT2RTR instead returned 0 on
// every connection of the first real capture, while rlid from the same pointer
// decoded correctly as 17 — the path was right, the field is simply not set
// there. src_lid came back 0 everywhere as a result, which is the collision
// (src_lid, qp_num) exists to prevent, reintroduced.
static std::mutex                                       g_lid_mutex;
static std::unordered_map<std::uint32_t, std::uint32_t> g_qp_src_lid;

static void remember_src_lid(std::uint32_t qp_num, std::uint32_t lid) {
    std::lock_guard<std::mutex> lk(g_lid_mutex);
    g_qp_src_lid[qp_num] = lid;
}

static std::uint32_t src_lid_of(std::uint32_t qp_num) {
    std::lock_guard<std::mutex> lk(g_lid_mutex);
    auto                        it = g_qp_src_lid.find(qp_num);
    return it == g_qp_src_lid.end() ? 0u : it->second;
}

static void decode_rst2init(mlx5dv_devx_obj* obj, const void* cmd_in) {
    auto*       qpc = DEVX_ADDR_OF(rst2init_qp_in, const_cast<void*>(cmd_in), qpc);
    const auto  qp_num = DEVX_GET(rst2init_qp_in, cmd_in, qpn);
    const auto* path = DEVX_ADDR_OF(qpc, qpc, primary_address_path);
    const auto  port_num = DEVX_GET(ads, path, vhca_port_num);

    ibv_context* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_obj_mutex);
        auto                        it = g_obj_ctx.find(obj);
        if (it != g_obj_ctx.end())
            ctx = it->second;
    }
    remember_src_lid(qp_num, port_lid(ctx, static_cast<std::uint8_t>(port_num)));
}

// --- DEVX decoding ---------------------------------------------------------

static void decode_create_qp(mlx5dv_devx_obj* obj, const void* cmd_in, void* cmd_out) {
    const auto* qpc = DEVX_ADDR_OF(create_qp_in, const_cast<void*>(cmd_in), qpc);

    const auto qp_num = DEVX_GET(create_qp_out, cmd_out, qpn);
    const auto wq_umem_id = DEVX_GET(create_qp_in, cmd_in, wq_umem_id);
    const auto wq_offset = DEVX_GET64(create_qp_in, cmd_in, wq_umem_offset);
    const auto log_sq_size = DEVX_GET(qpc, qpc, log_sq_size);
    const auto service_type = DEVX_GET(qpc, qpc, st);
    // The send completion queue, which this decode has always had in front of it
    // and never read. It is what lets a captured send be joined to the queue the
    // NIC will report it in.
    const auto cqn_snd = DEVX_GET(qpc, qpc, cqn_snd);

    // Unlike SigmaTrace (Mlx5dv.cpp:82-87) this does NOT return early when the
    // WQ buffer is not GPU-resident. A CPU-side DEVX queue pair is still a
    // queue pair, still carries topology, and is still invisible to the ibverbs
    // provider — dropping it is how a capture comes back empty and looks like a
    // workload that made no connections.
    UmemInfo   wq{};
    const bool known = lookup_umem(wq_umem_id, &wq);

    // GPU residency, and nothing else. This used to read
    //
    //     known && (wq.device != kUnknownDevice || wq.binding == ByDmabuf)
    //
    // because under dmabuf there was no address to ask CUDA about, so "it
    // arrived as a dmabuf" stood in for "it is on a GPU". That made case 09's
    // ibgda_engaged gate circular: the gate whose comment claims it proves the
    // GPU can write the send queue was in fact asserting the binding, which
    // umem_binding_as_configured already asserts separately. No by_dmabuf case
    // could ever have falsified it.
    //
    // dmabuf umems now carry a real device ordinal, recovered from the export
    // call, so the fallback is no longer load-bearing and is gone. The cost is
    // deliberate: if the export is not observed, this reports false rather than
    // true, and ibgda_engaged fails. That is the honest answer — we could not
    // establish GPU residency — and a red board saying so is worth more than a
    // green one that assumed it.
    const bool gpu_driven = known && wq.device != kUnknownDevice;

    // The other half of the UAR-selection measurement above: which UAR page this
    // queue pair will ring, and whether it is one this provider has any business
    // watching. Joining the two logs answers whether alloc-time `flags` can tell
    // IBGDA's UARs from UCX's — which is what a correct arm() needs and does not
    // have. There is no uar_page field on Mlx5DevxQp to make this a SQL join
    // instead; adding one is the follow-up if the flags turn out to separate.
    if (hostdb::debug_on())
        std::fprintf(stderr, "[hostdb] CREATE_QP qpn=%u uar_page=%u gpu_driven=%d\n", qp_num,
                     DEVX_GET(qpc, qpc, uar_page), gpu_driven ? 1 : 0);

    rec().record(pcomm::core::Temporal::none(),
                 rr::Mlx5DevxQp{{},
                                {},
                                h(obj),
                                known ? wq.addr + wq_offset : 0,
                                qp_num,
                                wq_umem_id,
                                log_sq_size,
                                known ? wq.device : kUnknownDevice,
                                cqn_snd,
                                static_cast<std::uint8_t>(service_type),
                                static_cast<std::uint8_t>(gpu_driven ? 1 : 0)});

    // Hand the doorbell hijack this queue pair's send-queue GPU address, so its
    // repeater can copy WQEs out of it. The recorded sq_base above stays an
    // offset under dmabuf; the real GPU VA is wq.gpu_base + wq_offset.
    if ((hijack::enabled() || hostdb::enabled()) && gpu_driven) {
        hijack::register_qp(qp_num, wq.gpu_base + wq_offset, log_sq_size,
                            static_cast<std::uint8_t>(service_type));
        // The host path has no UAR swap on an application thread, so this is
        // where it captures the CUDA context its copy-out will bind to. Free
        // for the GPU hijack, which already has one by now.
        if (hostdb::enabled())
            hijack::capture_gpu_ctx();
    }

    // The passive send-queue watcher (sq_watch.hpp): a CPU-driven queue pair
    // whose send queue and doorbell record are plain host memory, which is
    // ibdevx's shape exactly. IBGDA's queues are GPU-resident and stay with the
    // doorbell paths above.
    if (sqwatch::enabled() && !gpu_driven) {
        // A regular receive queue shares the WQ buffer and comes FIRST, so the
        // send queue starts after it. ibdevx uses an SRQ, where it is zero bytes —
        // but a queue pair from another stack need not.
        const auto          rq_type = DEVX_GET(qpc, qpc, rq_type);
        const std::uint64_t rq_bytes = rq_type == 0
                                           ? (std::uint64_t{1} << DEVX_GET(qpc, qpc, log_rq_size))
                                                 << (DEVX_GET(qpc, qpc, log_rq_stride) + 4)
                                           : 0;
        const std::uint64_t sq_off = wq_offset + rq_bytes;
        const std::uint64_t sq_bytes = (std::uint64_t{1} << log_sq_size) * hijack::kWqebb;
        const bool          dbr_valid = DEVX_GET(qpc, qpc, dbr_umem_valid) != 0;
        const auto          dbr_umem_id = DEVX_GET(qpc, qpc, dbr_umem_id);
        const std::uint64_t dbr_off = DEVX_GET64(qpc, qpc, dbr_addr);
        UmemInfo            dbr{};
        if (!known || wq.binding != UmemBinding::ByAddress || sq_off + sq_bytes > wq.size)
            sqwatch::refused(qp_num, "send queue is not a known host umem");
        else if (!dbr_valid || !lookup_umem(dbr_umem_id, &dbr) ||
                 dbr.binding != UmemBinding::ByAddress || dbr_off + 8 > dbr.size)
            sqwatch::refused(qp_num, "doorbell record is not a known host umem");
        else
            sqwatch::watch(qp_num, wq.addr + sq_off, log_sq_size,
                           static_cast<std::uint8_t>(service_type), dbr.addr + dbr_off, wq.umem,
                           dbr.umem);
    }
}

// The completion queue IBGDA polls, captured for its identity and geometry.
//
// It cannot capture a poll: `ibgda_poll_cq` reads the CQE with ordinary GPU
// loads and finishes with an atomicMax on a SOFTWARE consumer index in GPU
// memory. No doorbell record is written for a CQ on the device path — the only
// dbrec IBGDA touches is qp->tx_wq.dbrec, on the send side — so a consumed
// completion leaves no artifact anywhere a host interposer or the NIC can see.
// This records what the CQ IS, and the honest limit is stated on the record.
static void decode_create_cq(mlx5dv_devx_obj* obj, const void* cmd_in, void* cmd_out) {
    const auto* cqc = DEVX_ADDR_OF(create_cq_in, const_cast<void*>(cmd_in), cq_context);

    const auto cqn = DEVX_GET(create_cq_out, cmd_out, cqn);
    const auto umem_id = DEVX_GET(create_cq_in, cmd_in, cq_umem_id);
    const auto umem_offset = DEVX_GET64(create_cq_in, cmd_in, cq_umem_offset);
    const auto log_cq_size = DEVX_GET(cqc, cqc, log_cq_size);
    const auto dbr_umem_id = DEVX_GET(cqc, cqc, dbr_umem_id);
    const auto cqe_sz = DEVX_GET(cqc, cqc, cqe_sz);

    // Same rule as the queue pair: a CPU-side CQ is still a CQ and is still
    // invisible to the ibverbs provider, so it is recorded either way and
    // residency is a field rather than a filter.
    UmemInfo   cq{};
    const bool known = lookup_umem(umem_id, &cq);

    rec().record(pcomm::core::Temporal::none(),
                 rr::Mlx5DevxCq{{},
                                {},
                                h(obj),
                                known ? cq.gpu_base + umem_offset : 0,
                                cqn,
                                umem_id,
                                log_cq_size,
                                dbr_umem_id,
                                known ? cq.device : kUnknownDevice,
                                static_cast<std::uint8_t>(
                                    (known && cq.device != kUnknownDevice) ? 1 : 0),
                                static_cast<std::uint8_t>(cqe_sz)});
}

static void decode_init2rtr(mlx5dv_devx_obj* obj, const void* cmd_in) {
    auto*      qpc = DEVX_ADDR_OF(init2rtr_qp_in, const_cast<void*>(cmd_in), qpc);
    const auto qp_num = DEVX_GET(init2rtr_qp_in, cmd_in, qpn);
    const auto dest_qp_num = DEVX_GET(qpc, qpc, remote_qpn);

    const auto* path = DEVX_ADDR_OF(qpc, qpc, primary_address_path);
    const auto  dest_lid = DEVX_GET(ads, path, rlid);
    const auto  port_num = DEVX_GET(ads, path, vhca_port_num);

    const auto* rgid = reinterpret_cast<const std::uint8_t*>(
        DEVX_ADDR_OF(ads, const_cast<std::uint8_t*>(path), rgid_rip));
    std::uint64_t gid_prefix = 0, gid_iface = 0;
    std::memcpy(&gid_prefix, rgid, 8);
    std::memcpy(&gid_iface, rgid + 8, 8);

    // src_lid comes from the RST2INIT we already saw for this queue pair, not
    // from this command — see decode_rst2init. port_num is still reported from
    // here so a capture shows what INIT2RTR actually carried (0 on this stack)
    // rather than silently borrowing the INIT value and looking consistent.
    rec().record(rr::Mlx5DevxQpConnect{{},
                                       {},
                                       be64toh(gid_prefix),
                                       be64toh(gid_iface),
                                       qp_num,
                                       dest_qp_num,
                                       static_cast<std::uint32_t>(dest_lid),
                                       src_lid_of(qp_num),
                                       static_cast<std::uint8_t>(port_num)});
}

Profiler::Profiler() {
    // Watch for the dmabuf export BEFORE anything else, because it happens
    // before the registration that needs it. NVSHMEM resolves CUDA lazily, so
    // this only has to be registered before its first cuGetProcAddress, not
    // before CUDA loads.
    //
    // Registering costs nothing on a host with no CUDA: the symbols are simply
    // never resolved, the callbacks never run, and every dmabuf umem reports the
    // unknown device it reported before.
    pcomm::core::dmabuf::install();

    // The doorbell hijack (cudaHost* interposers below) is opt-in and OFF by
    // default: it is the one pcomm mechanism that sits in the application's data
    // path. The exported interposers pass through untouched unless enabled; here
    // we only announce it, because from an enabled run the timing is a
    // measurement of the instrumented system, not the application.
    // BOTH doorbell mechanisms share this decoder, so BOTH need its sinks. That
    // is not a tidiness point: with the sinks installed only for the GPU hijack,
    // a host-doorbell run decoded 381 doorbells on MN5 (job 45449944) and wrote
    // ZERO rows — on_doorbell incremented its counters, found a null sink, and
    // returned. The capture had no stream 212 and no 214 while the shutdown line
    // claimed hundreds of doorbells, which is the one contradiction this
    // provider must never ship.
    if (hijack::enabled() || hostdb::enabled() || sqwatch::enabled()) {
        // The decode runs on the repeater thread and emits through these sinks,
        // because the Recorder and intern tables live here in the provider. The
        // doorbell's uar handle interns h(uar_ptr) — the same handle the Mlx5Uar
        // entity carries — so the two join. Interning off the app thread is safe:
        // the engine's intern tables are process-wide under a per-domain lock.
        hijack::set_sinks(
            [](void* uar_ptr, std::uint32_t qp_num, std::uint32_t prod_idx) {
                rec().record(rr::Mlx5Doorbell{{}, {}, h(uar_ptr), qp_num, prod_idx});
            },
            [](std::uint32_t qp_num, std::uint32_t wqe_idx, std::uint8_t opcode,
               std::uint64_t laddr, std::uint32_t lkey, std::uint64_t raddr, std::uint32_t rkey,
               std::uint32_t length, std::uint32_t dest_dct, std::uint8_t decode,
               std::uint8_t av_ds) {
                rec().record(rr::Mlx5DeviceSend{{}, {}, laddr, raddr, qp_num, wqe_idx, lkey, rkey,
                                                length, dest_dct, opcode, decode, av_ds});
            });
    }

    // ...but the CUDA export-table patcher is the GPU path's alone. The host
    // path must not install it: patching the driver is how the GPU hijack sees
    // the UAR mapping, and there is no such mapping in the mode the host path
    // exists for.
    if (hijack::enabled()) {
        hijack::install();
        std::fprintf(stderr, "[pcomm] doorbell hijack ENABLED — this run sits in the data "
                             "path and its timing is perturbed; do not draw latency "
                             "conclusions from it\n");
    }

    // Bring up sink + recorder. `Tracer::instance()` alone was not enough and
    // failed in the most misleading way available: every hook fired (LD_DEBUG
    // confirmed nvshmem_transport_ibgda.so binding to this .so for umem_reg_ex,
    // alloc_uar and obj_create), every record was constructed, and the capture
    // directory came back empty because no engine had been acquired to hold a
    // sink. "The provider captured nothing" looked exactly like "the hooks do
    // not work", which is the wrong half of the system to go debugging.
    const pcomm::Config cfg = pcomm::Config::from_env();
    // Declare before acquiring: checks our ABI against the engine's and that
    // nobody else has claimed 200-299. That matters more here than for ibverbs,
    // because this provider is designed to co-run with it and pcomm-read merges
    // provider streams assuming the ranges are disjoint.
    if (pcomm_engine_declare_provider("mlx5dv", pcomm::core::id_range::mlx5dv_lo,
                                      pcomm::core::id_range::mlx5dv_hi,
                                      PCOMM_ENGINE_ABI_VERSION) != 0)
        return;  // the engine has already explained why on stderr
    pcomm_engine_acquire(cfg.output_dir.c_str(), cfg.rank, cfg.verbose ? 1 : 0);

    this->register_post<devx_umem_reg_tag>(
        [](mlx5dv_devx_umem* out, ibv_context*, void* addr, size_t size, uint32_t access, auto...) {
            record_umem(out, addr, size, access, -1, UmemEntry::Reg);
        });

    // The entry point IBGDA actually uses on current NVSHMEM. `addr` here is an
    // offset into the dmabuf when one is supplied, not a pointer — which is why
    // the binding is recorded rather than the address alone being trusted.
    this->register_post<devx_umem_reg_ex_tag>(
        [](mlx5dv_devx_umem* out, ibv_context*, mlx5dv_devx_umem_in* in, auto...) {
            if (!in)
                return;
            const bool has_dmabuf = (in->comp_mask & MLX5DV_UMEM_MASK_DMABUF) != 0;
            record_umem(out, in->addr, in->size, in->access, has_dmabuf ? in->dmabuf_fd : -1,
                        UmemEntry::RegEx);
        });

    this->register_post<devx_alloc_uar_tag>(
        [](mlx5dv_devx_uar* out, ibv_context*, uint32_t flags, auto...) {
            if (!out)
                return;
            // The hijack only ever swaps a page that came from here — this is
            // the "never manufacture a capability" rule made concrete. The uar
            // pointer is kept so a doorbell row can join back to Mlx5Uar (h(out)).
            if (hijack::enabled() || hostdb::enabled())
                hijack::remember_uar(out->reg_addr, out, out->page_id);
            // ...and only then swap, so remember_uar keys on the register the
            // NIC really owns and a doorbell row joins the Mlx5Uar recorded
            // below. The record therefore carries the true reg_addr in both
            // modes; the decoy is an instrument and never appears in a capture.
            //
            // ...and only for a UAR IBGDA asked for. Arming every UAR the
            // process allocates swaps UCX's too, and a repeater that forwards
            // eight bytes from offset 0 truncates the BlueFlame writes UCX's RC
            // transport rings — its sends stop reaching the NIC and a two-node
            // run hangs in MPI_Comm_dup inside NVSHMEM's bootstrap. The flags
            // separate the two cleanly; is_ibgda_uar has the measurement.
            if (hostdb::enabled() && hostdb::is_ibgda_uar(flags))
                hostdb::arm(out);
            // The allocation flags, which are the one thing known at this moment
            // that might distinguish a UAR IBGDA will use from one UCX will —
            // the question arming has to answer and currently does not, so it
            // arms both and breaks MPI (things-to-circle-back-to.md). Printed
            // rather than only recorded because the QP side below has no record
            // field to join to yet, so the correlation lives in the log.
            if (hostdb::debug_on())
                std::fprintf(stderr, "[hostdb] UAR alloc page_id=%u flags=0x%x reg_addr=%p\n",
                             out->page_id, flags, out->reg_addr);
            rec().record(pcomm::core::Temporal::none(),
                         rr::Mlx5Uar{{},
                                     {},
                                     h(out),
                                     reinterpret_cast<std::uint64_t>(out->reg_addr),
                                     reinterpret_cast<std::uint64_t>(out->base_addr),
                                     out->page_id,
                                     flags});
        });

    // devx_umem_dereg / devx_free_uar -> the entity's lifetime ended.
    //
    // free_uar returns void, which is why it could not be hooked at all until
    // the engine grew a void specialisation — a UAR lifetime was unrecordable
    // for a reason that had nothing to do with UARs.
    //
    // The subject is the same interned pointer the open record carries
    // (Mlx5Umem h(out), Mlx5Uar h(out)), so the join is exact.
    this->register_post<devx_umem_dereg_tag>([](int ret, mlx5dv_devx_umem* umem, auto...) {
        if (ret != 0 || !umem)
            return;
        // Before the transport frees the buffer: the watcher's last look at a
        // send queue or doorbell record, and then it lets go of both.
        if (sqwatch::enabled())
            sqwatch::unwatch_umem(umem);
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{
                                              {}, {}, h(umem), rr::Mlx5Umem::pcomm_type_id});
    });
    this->register_post<devx_free_uar_tag>([](mlx5dv_devx_uar* uar, auto...) {
        if (!uar)
            return;
        // Stop the repeater before the page goes away. Idempotent with the
        // teardown cuMemHostUnregister does, in whichever order they arrive.
        if (hijack::enabled())
            hijack::teardown(uar->reg_addr);
        if (hostdb::enabled())
            hostdb::disarm(uar);
        rec().record(rec().now_instant(),
                     pcomm::core::records::EntityClose{{}, {}, h(uar), rr::Mlx5Uar::pcomm_type_id});
    });

    this->register_post<devx_obj_create_tag>([](mlx5dv_devx_obj* obj, ibv_context* ctx,
                                                const void* cmd_in, size_t inlen, void* cmd_out,
                                                size_t, auto...) {
        if (!obj || !cmd_in || inlen < 4)
            return;
        {
            std::lock_guard<std::mutex> lk(g_obj_mutex);
            g_obj_ctx[obj] = ctx;
        }
        const auto opcode = DEVX_GET(create_qp_in, cmd_in, opcode);
        const bool decode =
            ((opcode == MLX5_CMD_OP_CREATE_QP) || (opcode == MLX5_CMD_OP_CREATE_CQ)) && cmd_out;
        if (opcode == MLX5_CMD_OP_CREATE_QP && cmd_out)
            decode_create_qp(obj, cmd_in, cmd_out);
        else if (opcode == MLX5_CMD_OP_CREATE_CQ && cmd_out)
            decode_create_cq(obj, cmd_in, cmd_out);
        // Every command is recorded, decoded or not. An opcode this build does
        // not know is the version boundary most likely to move under us, and it
        // must be visible in the capture rather than absent from it.
        rec().record(rr::Mlx5DevxCmd{{},
                                     {},
                                     h(obj),
                                     static_cast<std::uint32_t>(opcode),
                                     decode ? std::uint8_t{1} : std::uint8_t{0}});
    });

    // mlx5dv_create_qp -> the queue-pair entity the ibverbs provider cannot see.
    //
    // perftest reaches its queue pairs through here and never calls
    // ibv_create_qp, so ib_queue_pair has been empty on case 08 since the case
    // was written — with three green goldens beside it and three modify_qp
    // transitions referring to a queue pair no record had created. The entity
    // goes in this provider's range because a provider may only write inside
    // the range it declared, which the engine enforces.
    this->register_post<create_qp_tag>(
        [](ibv_qp* qp, ibv_context*, ibv_qp_init_attr_ex* attr, mlx5dv_qp_init_attr*, auto...) {
            if (!qp)
                return;
            rec().record(
                pcomm::core::Temporal::none(),
                rr::Mlx5QueuePair{{},
                                  {},
                                  h(qp),
                                  h(attr ? attr->pd : nullptr),
                                  qp->qp_num,
                                  static_cast<std::uint8_t>(attr ? attr->qp_type : qp->qp_type)});
            // ibv_destroy_qp is the engine's (this provider and ibverbs coexist,
            // and both create queue pairs it tears down). Registering the handle
            // is what makes the close name mlx5_queue_pair — the door that
            // actually created it — instead of ib_queue_pair.
            pcomm_engine_note_qp(qp, rr::Mlx5QueuePair::pcomm_type_id);
        });

    this->register_post<devx_obj_modify_tag>([](int ret, mlx5dv_devx_obj* obj, const void* cmd_in,
                                                size_t inlen, void*, size_t, auto...) {
        if (ret != 0 || !obj || !cmd_in || inlen < 4)
            return;
        const auto opcode = DEVX_GET(init2rtr_qp_in, cmd_in, opcode);
        const bool decode =
            (opcode == MLX5_CMD_OP_INIT2RTR_QP || opcode == MLX5_CMD_OP_RST2INIT_QP);
        if (opcode == MLX5_CMD_OP_RST2INIT_QP)
            decode_rst2init(obj, cmd_in);  // learn the port -> our LID
        else if (opcode == MLX5_CMD_OP_INIT2RTR_QP)
            decode_init2rtr(obj, cmd_in);
        rec().record(rr::Mlx5DevxCmd{{},
                                     {},
                                     h(obj),
                                     static_cast<std::uint32_t>(opcode),
                                     decode ? std::uint8_t{1} : std::uint8_t{0}});
    });
}

}  // namespace mlx5

template class mlx5::Mlx5dvEventSource<mlx5::Profiler>;

// --- LD_PRELOAD interposers --------------------------------------------------

extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg(ibv_context* ctx, void* addr, size_t size,
                                                  uint32_t access) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_umem_reg_tag>(ctx, addr, size, access);
}
extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg_ex(ibv_context*         ctx,
                                                     mlx5dv_devx_umem_in* umem_in) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_umem_reg_ex_tag>(ctx, umem_in);
}
extern "C" int mlx5dv_devx_umem_dereg(mlx5dv_devx_umem* umem) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_umem_dereg_tag>(umem);
}
extern "C" mlx5dv_devx_uar* mlx5dv_devx_alloc_uar(ibv_context* ctx, uint32_t flags) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_alloc_uar_tag>(ctx, flags);
}
extern "C" void mlx5dv_devx_free_uar(mlx5dv_devx_uar* uar) {
    mlx5::Profiler::invoke_preload<mlx5::devx_free_uar_tag>(uar);
}
extern "C" mlx5dv_devx_obj* mlx5dv_devx_obj_create(ibv_context* ctx, const void* in, size_t inlen,
                                                   void* out, size_t outlen) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_obj_create_tag>(ctx, in, inlen, out, outlen);
}
extern "C" int mlx5dv_devx_obj_modify(mlx5dv_devx_obj* obj, const void* in, size_t inlen, void* out,
                                      size_t outlen) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_obj_modify_tag>(obj, in, inlen, out, outlen);
}
extern "C" ibv_qp* mlx5dv_create_qp(ibv_context* ctx, ibv_qp_init_attr_ex* attr,
                                    mlx5dv_qp_init_attr* mlx5_attr) {
    return mlx5::Profiler::invoke_preload<mlx5::create_qp_tag>(ctx, attr, mlx5_attr);
}

// The doorbell hijack's CUDA driver interceptions are NOT exported symbols: they
// are patched directly into libcuda's private export table (cuda_export_table.hpp),
// because on du04 NVSHMEM links libcudart statically and reaches the driver only
// that way. Nothing to interpose here.

// --- provider registration ---------------------------------------------------

namespace pcomm::providers {

class Mlx5dvProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"mlx5dv", core::id_range::mlx5dv_lo, core::id_range::mlx5dv_hi};
    }
    bool init(const provider::ProviderConfig&, rec::Recorder<>&) override {
        mlx5::Profiler::instance();
        return true;
    }
    void shutdown() override {
        pcomm_engine_drain_thread();
        pcomm_engine_release();
    }
};

}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::Mlx5dvProvider)

static void __attribute__((constructor)) pcomm_mlx5dv_load() {
    mlx5::Profiler::instance();
}
static void __attribute__((destructor)) pcomm_mlx5dv_unload() {
    if (mlx5::sqwatch::enabled())
        mlx5::sqwatch::shutdown(pcomm::Config::from_env().rank);
    if (mlx5::hijack::enabled())
        mlx5::hijack::report_and_shutdown(pcomm::Config::from_env().rank);
    if (mlx5::hostdb::enabled())
        mlx5::hostdb::report_and_shutdown(pcomm::Config::from_env().rank);
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
