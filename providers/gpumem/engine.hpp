#pragma once
// gpumem engine — the vendor-free half of the provider. The GPU backend
// (gpu-backends/nvbit) translates vendor callbacks into these calls; the
// engine owns the attribution pipeline and writes pcomm records.
//
// pc identity: the backend assigns each instrumented instruction a dense
// global index; handle = index + 1 (0 stays null). GpuAccess.pc and
// SourceLine.pc therefore join without interning on the access path.

#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "pcomm/attr/pipeline.hpp"
#include "pcomm/core/ids.hpp"
#include "pcomm/core/records.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/tracer.hpp"

namespace pcomm::gpumem {

inline core::Handle pc_handle(std::uint32_t instr_index) {
    return instr_index + 1;
}

// Tracks live device allocations: resolves an address to (device, object).
// Fed by the backend's alloc callbacks; queried per access on the drain path.
class AllocRegistry {
public:
    struct Alloc {
        std::uint64_t  base;
        std::uint64_t  size;
        core::DeviceId device;
        core::Handle   object;  // MemObject entity id
    };

    // `object` is minted by the caller, not here, and that is the whole point:
    // it used to be a dense local counter (`m_allocs.size() + 1`), which made
    // MemObject.id provider-local and unjoinable — gpumem's object 3 and an
    // interned pointer handle 3 were the same uint32 naming different buffers.
    // on_alloc now interns the base address in the same domain ibverbs and
    // mlx5dv use, so the id is a shared identity across providers.
    void add(std::uint64_t base, std::uint64_t size, core::DeviceId dev, core::Handle object) {
        std::lock_guard lock(m_mutex);
        m_allocs.push_back({base, size, dev, object});
    }

    // A range that resolves a device but names no MemObject — the backend's
    // view of another PE's NVSHMEM symmetric heap, mapped into this process.
    // Deliberately not add(): that would mint an object handle for an entity
    // nobody recorded, leaving GpuAccess.object pointing at a MemObject row
    // that does not exist. Ownership is the only thing we know here.
    void add_range(std::uint64_t base, std::uint64_t size, core::DeviceId dev) {
        std::lock_guard lock(m_mutex);
        m_allocs.push_back({base, size, dev, core::kNullHandle});
    }

    // Returns {found, alloc}; linear scan like snoopie's find_dev_of_ptr
    // (allocation counts are small; interval tree later if needed).
    const Alloc* find(std::uint64_t addr) const {
        std::lock_guard lock(m_mutex);
        for (const auto& a : m_allocs) {
            if (a.base <= addr && addr < a.base + a.size)
                return &a;
        }
        return nullptr;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Alloc> m_allocs;
};

// Fills GpuAccess.object from the alloc registry (Snoopie's object
// attribution, minimal form: the identity link; var-name/callstack
// enrichment comes with the adm port).
class ObjectAttributor final : public attr::IAttributor {
public:
    explicit ObjectAttributor(const AllocRegistry& allocs) : m_allocs(&allocs) {}

    void attribute(const attr::RawAccess& a, core::records::GpuAccess& out,
                   rec::Recorder<>&) override {
        if (const auto* alloc = m_allocs->find(a.address))
            out.object = alloc->object;
    }

private:
    const AllocRegistry* m_allocs;
};

class RemoteFilter final : public attr::IFilter {
public:
    bool keep(const attr::RawAccess& a) const override { return a.run_device != a.mem_device; }
};

class Engine {
public:
    // Starts the Tracer (env config) and builds the pipeline.
    Engine() {
        // C ABI, not Tracer::start(Config): Config carries a std::filesystem::path
        // and this file is compiled by nvcc/gcc-11.4 while the engine is gcc-13.2.
        // from_env() itself is fine here — it only reads env vars locally; only
        // POD crosses the boundary.
        const Config cfg = Config::from_env();
        // See ibverbs: declare the 700-799 range and our ABI before acquiring.
        if (pcomm_engine_declare_provider("gpumem", core::id_range::gpumem_lo,
                                          core::id_range::gpumem_hi, PCOMM_ENGINE_ABI_VERSION) != 0)
            return;
        pcomm_engine_acquire(cfg.output_dir.c_str(), cfg.rank, cfg.verbose ? 1 : 0);
        auto& rec = recorder();
        m_pipeline = std::make_unique<attr::AttributionPipeline>(rec);
        m_pipeline->add_filter(std::make_unique<RemoteFilter>());
        m_pipeline->add_attributor(std::make_unique<ObjectAttributor>(m_allocs));
    }

    // A local, stateless proxy — NOT a reference into the engine's Recorder.
    // All its operations are inline calls into the C ABI.
    rec::Recorder<>& recorder() {
        static rec::Recorder<> proxy;
        return proxy;
    }
    AllocRegistry& allocs() { return m_allocs; }

    // Backend callbacks ------------------------------------------------------

    void on_alloc(std::uint64_t base, std::uint64_t size, core::DeviceId dev) {
        auto& rec = recorder();
        // The buffer's base address, interned in the Pointer domain — exactly
        // what ibverbs and mlx5dv do for the same buffer, so all three mint the
        // SAME id and a GpuAccess can resolve to a region ibverbs registered.
        // That was impossible while this was a dense counter local to gpumem.
        const auto object = rec.intern(rec::InternDomain::Pointer, base);
        m_allocs.add(base, size, dev, object);
        rec.record(rec.now_instant(),  // lifetime open until free-tracking lands
                   core::records::MemObject{{}, {}, object, core::kNullHandle, {base, size}, dev});
    }

    // One row per instrumented instruction, at instrumentation time.
    void on_source_line(std::uint32_t instr_index, std::string_view file, std::string_view dir,
                        std::uint32_t line) {
        auto&       rec = recorder();
        std::string path =
            dir.empty() ? std::string(file) : std::string(dir) + "/" + std::string(file);
        rec.record(
            core::Temporal::none(),
            core::records::SourceLine{
                {}, {}, pc_handle(instr_index), rec.intern(rec::InternDomain::String, path), line});
    }

    // Drain path: the backend hands raw accesses with run_device set and
    // mem_device unresolved; the engine resolves every one of them the same
    // way, via the alloc registry (still-unknown -> run_device, so RemoteFilter
    // drops it). NVSHMEM symmetric-heap ranges are registry entries like any
    // other allocation — the backend used to resolve those itself, from a guess
    // at NVSHMEM's VA layout, which is what this pipeline no longer contains.
    void process(std::span<attr::RawAccess> batch) {
        for (auto& a : batch) {
            if (a.mem_device != attr::kUnresolvedDevice)
                continue;
            const auto* alloc = m_allocs.find(a.address);
            a.mem_device = alloc ? alloc->device : a.run_device;
        }
        m_pipeline->process(batch);
    }

    // Buffers do not auto-drain at thread exit (leaked TLS, see Recorder) —
    // every recording thread must call this before it exits.
    void drain_thread() { recorder().drain_thread(); }

    // Called from the backend once the last context is gone (which may be
    // inside an exit handler — everything reachable from here is leak-safe).
    void shutdown() {
        drain_thread();
        pcomm_engine_release();
    }

private:
    AllocRegistry                              m_allocs;
    std::unique_ptr<attr::AttributionPipeline> m_pipeline;
};

}  // namespace pcomm::gpumem
