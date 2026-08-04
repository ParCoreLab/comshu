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

#include <dlfcn.h>
#include <endian.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

#include <mlx5/mlx5_ifc.h>

#include "pcomm/core/ids.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/provider/provider.hpp"
#include "pcomm/rec/intern_table.hpp"
#include "pcomm/tracer.hpp"

#include "mlx5_event_source.hpp"
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
// Resolved at runtime rather than linked. This provider must build and run on a
// host with no CUDA at all (MN5's core configuration does exactly that), and a
// DEVX capture without device attribution is still a useful capture. If the
// driver is absent every buffer is simply reported as kUnknownDevice, which is
// a distinct value from "device 0" on purpose.
namespace cuda {

using PtrAttrFn = int (*)(void*, int, unsigned long long);
constexpr int kAttrMemoryType = 2;     // CU_POINTER_ATTRIBUTE_MEMORY_TYPE
constexpr int kAttrDeviceOrdinal = 4;  // CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL
constexpr int kMemoryTypeDevice = 2;   // CU_MEMORYTYPE_DEVICE

static PtrAttrFn resolve() {
    static PtrAttrFn fn = [] {
        void* lib = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_NOLOAD);
        return lib ? reinterpret_cast<PtrAttrFn>(dlsym(lib, "cuPointerGetAttribute")) : nullptr;
    }();
    return fn;
}

// Owning device of a host-visible address, or kUnknownDevice.
static pcomm::core::DeviceId device_of(const void* addr) {
    auto* fn = resolve();
    if (!fn || !addr)
        return kUnknownDevice;
    unsigned int type = 0;
    if (fn(&type, kAttrMemoryType, reinterpret_cast<unsigned long long>(addr)) != 0)
        return kUnknownDevice;  // not a CUDA pointer: host memory
    if (static_cast<int>(type) != kMemoryTypeDevice)
        return kUnknownDevice;
    int ordinal = -1;
    if (fn(&ordinal, kAttrDeviceOrdinal, reinterpret_cast<unsigned long long>(addr)) != 0)
        return kUnknownDevice;
    return static_cast<pcomm::core::DeviceId>(ordinal);
}

}  // namespace cuda

// --- umem registry ---------------------------------------------------------
//
// DEVX command buffers name buffers by umem_id, so decoding CREATE_QP into a
// send-queue address requires remembering what each id was. Kept here rather
// than reconstructed by the reader because the reader would have to join across
// streams and would still not know the owning device.
struct UmemInfo {
    std::uint64_t             addr{0};
    std::uint64_t             size{0};
    pcomm::core::DeviceId     device{kUnknownDevice};
    UmemBinding               binding{UmemBinding::ByAddress};
    std::int32_t              dmabuf_fd{-1};
};

static std::mutex                                    g_umem_mutex;
static std::unordered_map<std::uint32_t, UmemInfo>   g_umems;
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
    UmemInfo   info{reinterpret_cast<std::uint64_t>(addr),
                    size,
                    by_dmabuf ? kUnknownDevice : cuda::device_of(addr),
                    by_dmabuf ? UmemBinding::ByDmabuf : UmemBinding::ByAddress,
                    dmabuf_fd};
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
}

// Our own port's LID, so (src_lid, qp_num) names this queue pair uniquely.
static std::uint32_t port_lid(ibv_context* ctx, std::uint8_t port_num) {
    if (!ctx || port_num == 0)
        return 0;
    ibv_port_attr pa{};
    return ibv_query_port(ctx, port_num, &pa) == 0 ? pa.lid : 0u;
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
    UmemInfo wq{};
    const bool known = lookup_umem(wq_umem_id, &wq);
    const bool gpu_driven = known && (wq.device != kUnknownDevice ||
                                      wq.binding == UmemBinding::ByDmabuf);

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

    ibv_context* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_obj_mutex);
        auto                        it = g_obj_ctx.find(obj);
        if (it != g_obj_ctx.end())
            ctx = it->second;
    }

    rec().record(rr::Mlx5DevxQpConnect{{},
                                       {},
                                       be64toh(gid_prefix),
                                       be64toh(gid_iface),
                                       qp_num,
                                       dest_qp_num,
                                       static_cast<std::uint32_t>(dest_lid),
                                       port_lid(ctx, static_cast<std::uint8_t>(port_num)),
                                       static_cast<std::uint8_t>(port_num)});
}

Profiler::Profiler() {
    pcomm::Tracer::instance();

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
            record_umem(out, in->addr, in->size, in->access,
                        has_dmabuf ? in->dmabuf_fd : -1, UmemEntry::RegEx);
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
        rec().record(rr::Mlx5DevxCmd{
            {}, {}, h(obj), static_cast<std::uint32_t>(opcode), decode ? std::uint8_t{1} : std::uint8_t{0}});
    });

    this->register_post<devx_obj_modify_tag>(
        [](int ret, mlx5dv_devx_obj* obj, const void* cmd_in, size_t inlen, void*, size_t, auto...) {
            if (ret != 0 || !obj || !cmd_in || inlen < 4)
                return;
            const auto opcode = DEVX_GET(init2rtr_qp_in, cmd_in, opcode);
            const bool decode = (opcode == MLX5_CMD_OP_INIT2RTR_QP);
            if (decode)
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
extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg_ex(ibv_context* ctx,
                                                     mlx5dv_devx_umem_in* umem_in) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_umem_reg_ex_tag>(ctx, umem_in);
}
extern "C" int mlx5dv_devx_umem_dereg(mlx5dv_devx_umem* umem) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_umem_dereg_tag>(umem);
}
extern "C" mlx5dv_devx_uar* mlx5dv_devx_alloc_uar(ibv_context* ctx, uint32_t flags) {
    return mlx5::Profiler::invoke_preload<mlx5::devx_alloc_uar_tag>(ctx, flags);
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
