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
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <endian.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

#include <mlx5/mlx5_ifc.h>

#include "pcomm/core/ids.hpp"
#include "pcomm/core/mem_attr.hpp"
#include "pcomm/core/records.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/tracer.hpp"

#include "mlx5_event_source.hpp"
#include "cuda_dmabuf.hpp"
#include "mlx5_records.hpp"

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
    // step earlier and dmabuf::lookup carries that here. It feeds two things
    // that were previously impossible, neither of which is the umem's address:
    //
    //   - the owning device, which gpu_driven otherwise has to infer from the
    //     binding alone (the heuristic that made case 09's gate circular);
    //   - a MemObject with a real range, so a dmabuf buffer becomes nameable.
    //
    // If the conversion was not observed, both stay unknown. That is the answer
    // this provider gave for every dmabuf umem until now, so a miss is a
    // regression to the previous behaviour rather than a new fiction.
    dmabuf::Range dmabuf_range{};
    const bool    resolved = by_dmabuf && dmabuf::lookup(dmabuf_fd, &dmabuf_range);

    UmemInfo info{reinterpret_cast<std::uint64_t>(addr), size,
                  by_dmabuf ? (resolved ? dmabuf_range.device : kUnknownDevice)
                            : pcomm::core::device_of(addr),
                  by_dmabuf ? UmemBinding::ByDmabuf : UmemBinding::ByAddress, dmabuf_fd};
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

    rec().record(pcomm::core::Temporal::none(),
                 rr::Mlx5DevxQp{{},
                                {},
                                h(obj),
                                known ? wq.addr + wq_offset : 0,
                                qp_num,
                                wq_umem_id,
                                log_sq_size,
                                known ? wq.device : kUnknownDevice,
                                static_cast<std::uint8_t>(service_type),
                                static_cast<std::uint8_t>(gpu_driven ? 1 : 0)});
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
    dmabuf::install();

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
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{{}, {}, h(umem)});
    });
    this->register_post<devx_free_uar_tag>([](mlx5dv_devx_uar* uar, auto...) {
        if (!uar)
            return;
        rec().record(rec().now_instant(), pcomm::core::records::EntityClose{{}, {}, h(uar)});
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
        const bool decode = (opcode == MLX5_CMD_OP_CREATE_QP) && cmd_out;
        if (decode)
            decode_create_qp(obj, cmd_in, cmd_out);
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
    pcomm_engine_drain_thread();
    pcomm_engine_release();
}
