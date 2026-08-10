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
#include <optional>
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
        // For a peer's mapping of a symmetric region: what to add to a local
        // address to get the SYMMETRIC one. Zero for everything else.
        //
        // It exists because nvshmem_malloc is a user-space sub-allocator over
        // the heap (src/host/mem/mem_heap.cpp -> mspace, a dlmalloc derivative),
        // so the fine-grained buffers only ever have symmetric addresses — no
        // CUDA call ever names them, and a peer access arrives at the mapping
        // address instead. Translating is the only way to reach them.
        std::int64_t sym_delta;
    };

    // `object` is minted by the caller, not here, and that is the whole point:
    // it used to be a dense local counter (`m_allocs.size() + 1`), which made
    // MemObject.id provider-local and unjoinable — gpumem's object 3 and an
    // interned pointer handle 3 were the same uint32 naming different buffers.
    // on_alloc now interns the base address in the same domain ibverbs and
    // mlx5dv use, so the id is a shared identity across providers.
    void add(std::uint64_t base, std::uint64_t size, core::DeviceId dev, core::Handle object,
             std::int64_t sym_delta = 0) {
        std::lock_guard lock(m_mutex);
        m_allocs.push_back({base, size, dev, object, sym_delta});
    }

    // A range that resolves a device but names no MemObject — the backend's
    // view of another PE's NVSHMEM symmetric heap, mapped into this process.
    // Deliberately not add(): that would mint an object handle for an entity
    // nobody recorded, leaving GpuAccess.object pointing at a MemObject row
    // that does not exist. Ownership is the only thing we know here.
    void add_range(std::uint64_t base, std::uint64_t size, core::DeviceId dev) {
        std::lock_guard lock(m_mutex);
        m_allocs.push_back({base, size, dev, core::kNullHandle, 0});
    }

    // Linear scan like snoopie's find_dev_of_ptr (allocation counts are small;
    // interval tree later if needed), but preferring a match that NAMES an
    // object over one that only resolves a device.
    //
    // The preference is load-bearing, not a tiebreak. The same peer mapping is
    // registered twice: once by the mapped-region pass, which sees a cuMemMap
    // and can only supply a device (add_range, null object), and again by the
    // NVSHMEM pass, which knows it is a symmetric region and supplies the
    // object. Returning the first hit meant the device-only entry always won —
    // and every recorded access stayed unattributed even after the object was
    // being registered correctly. Both entries agree on the device, so taking
    // the named one loses nothing.
    //
    // Among named matches, the SMALLEST wins. Ranges nest: an
    // nvshmem_malloc'd buffer sits inside the symmetric heap region that was
    // mapped once for all of them, and both name an object. The innermost is
    // the answer, for the same reason the innermost enclosing scope names a
    // variable — it is also the rule the reader's mem_owner already applies to
    // containment chains, so capture and reader agree.
    // BY VALUE, not by pointer. The lock ends with this call, so a pointer into
    // m_allocs would be read unlocked while another thread can push_back and
    // reallocate the vector — and close() below can erase the very element.
    // Copying a 40-byte POD is cheaper than either bug.
    std::optional<Alloc> find(std::uint64_t addr) const {
        std::lock_guard      lock(m_mutex);
        std::optional<Alloc> named, unnamed;
        for (const auto& a : m_allocs) {
            if (a.base <= addr && addr < a.base + a.size) {
                if (a.object != core::kNullHandle) {
                    if (!named || a.size < named->size)
                        named = a;
                } else if (!unnamed || a.size < unnamed->size) {
                    unnamed = a;
                }
            }
        }
        return named ? named : unnamed;
    }

    // The object registered at exactly this base, for closing the entity.
    //
    // It does NOT erase, and that is the correction to a bug this very function
    // introduced. Accesses are attributed on the recv thread as the channel
    // drains, which can happen after the kernel and after the application has
    // freed the buffer; dropping the entry at free time made those late
    // arrivals fall back to the enclosing heap region, and mem_owner regressed
    // from the 4 KB buffer to the 512 MB heap.
    //
    // Keeping the entry is also what the project already decided for every
    // other entity: lifetimes are expressed by appending an EntityClose record,
    // not by deleting state, so a reader can resolve an address reused across a
    // free with the [open, close) window. See
    // PLAN-20260805-0053-entity-lifetimes.
    //
    // Matching the exact base is deliberate: free() is given the pointer malloc
    // returned, and anything merely *containing* it is the heap region.
    core::Handle object_at(std::uint64_t base) const {
        std::lock_guard lock(m_mutex);
        for (const auto& a : m_allocs)
            if (a.base == base && a.object != core::kNullHandle)
                return a.object;
        return core::kNullHandle;
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
        // The core first: it interposes the allocator APIs, so it knows the
        // application's own buffers — which this registry cannot see, because
        // nvshmem_malloc sub-allocates from a pre-mapped heap and reaches no
        // CUDA call. Innermost wins there too, so a 4 KB buffer beats the
        // 512 MB region it sits in.
        std::uint64_t cbase = 0, csize = 0;
        std::uint32_t cobj = 0;
        std::uint16_t cdev = 0;
        if (pcomm_engine_object_at(a.address, &cbase, &csize, &cobj, &cdev) && cobj)
            out.object = cobj;

        const auto alloc = m_allocs->find(a.address);
        if (!alloc)
            return;
        if (out.object == core::kNullHandle)
            out.object = alloc->object;

        // A peer's mapping resolves to the region; the buffer inside it is only
        // addressable symmetrically, so translate and look again. Falls back to
        // the region's own object when the finer one is not known — an
        // application that never calls nvshmem_malloc, or a buffer allocated
        // before we interposed.
        if (alloc->sym_delta != 0) {
            const auto sym = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(a.address) + alloc->sym_delta);
            // The core's table is keyed by symmetric address, which is what the
            // translation produces — so a peer access lands on the very buffer
            // the application allocated, named identically on every PE.
            if (pcomm_engine_object_at(sym, &cbase, &csize, &cobj, &cdev) && cobj) {
                out.object = cobj;
            } else if (const auto fine = m_allocs->find(sym);
                       fine && fine->object != core::kNullHandle) {
                out.object = fine->object;
            }
        }
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

    // Record this process's own symmetric-heap region as a MemObject, and
    // return its handle so the peer mappings of the same region can name it.
    //
    // Why this was missing, and why gpu_access.object was 0 for every recorded
    // access: the symmetric heap is created through the VMM APIs (cuMemCreate /
    // cuMemMap), so it never reaches on_alloc, and the mapped-region pass files
    // it with add_range — which deliberately names no object. There was
    // therefore no MemObject anywhere for the one buffer every peer access
    // touches. RemoteFilter keeps only peer accesses, so *every* row we record
    // pointed at nothing.
    //
    // Interning the base in the Pointer domain is the same identity scheme
    // on_alloc, ibverbs and mlx5dv use, so this joins to the rest rather than
    // minting a private id.
    //
    // Deliberately does not add() the range again: the mapped-region pass has
    // already registered [base, base+size) and find() returns the first match,
    // so a second entry would be dead weight. Local accesses to it are filtered
    // out by RemoteFilter in any case — the handle exists for the peers.
    core::Handle on_symmetric_region(std::uint64_t base, std::uint64_t size, core::DeviceId dev) {
        auto&      rec = recorder();
        const auto object = rec.intern(rec::InternDomain::Pointer, base);
        rec.record(rec.now_instant(),
                   core::records::MemObject{{}, {}, object, core::kNullHandle, {base, size}, dev});
        return object;
    }

    // A peer's copy of a symmetric region, mapped into this process at
    // `mapped_base`, carrying the object handle of the local region it mirrors.
    //
    // Naming the *local* object for a *peer's* memory is correct rather than
    // approximate: the heap is symmetric, so the buffer at a given offset is the
    // same logical buffer on every PE. Measured on
    // nvshmem_multinode-44451168 — rank 0's object on device 0 and rank 1's on
    // device 1 are both at 1100585369600, while the peer mapping sits ~275 GB
    // away at 1375639449088. That distance is why containment against the raw
    // address finds nothing, and why the mapping has to carry the handle
    // instead of the reader trying to recover it.
    void on_peer_range(std::uint64_t mapped_base, std::uint64_t size, core::DeviceId dev,
                       core::Handle object, std::uint64_t symmetric_base) {
        m_allocs.add(mapped_base, size, dev, object,
                     static_cast<std::int64_t>(symmetric_base) -
                         static_cast<std::int64_t>(mapped_base));
    }

    // One nvshmem_malloc'd buffer, at its symmetric address.
    //
    // These never reach any CUDA allocation API: nvshmem_malloc sub-allocates
    // from the pre-mapped heap through NVSHMEM's own mspace allocator
    // (src/host/mem/mem_heap.cpp -> custom_malloc.cpp, a dlmalloc derivative),
    // so the driver only ever saw the one cuMemMap of the whole heap. Which is
    // why attribution stopped at 512 MB of "the heap" and why this has to be
    // interposed at the NVSHMEM boundary rather than below it.
    //
    // The address is symmetric, so interning it yields the SAME id on every PE —
    // the property that already makes the heap region a cross-rank identity,
    // now at buffer granularity.
    void on_symmetric_alloc(std::uint64_t base, std::uint64_t size, core::DeviceId dev) {
        if (!base || !size)
            return;
        auto&      rec = recorder();
        const auto object = rec.intern(rec::InternDomain::Pointer, base);
        rec.record(rec.now_instant(),
                   core::records::MemObject{{}, {}, object, core::kNullHandle, {base, size}, dev});
        m_allocs.add(base, size, dev, object);
    }

    // cuMemFree: a plain device allocation's lifetime ends here.
    //
    // Same shape as on_symmetric_free and deliberately a separate entry point:
    // the two arrive on different threads (a CUDA callback vs the application's
    // own call into NVSHMEM) and name buffers registered by different paths.
    // Sharing the body would only hide that.
    void on_free(std::uint64_t base) { close_object(base); }

    // nvshmem_free: the buffer's lifetime ends here.
    //
    // Without this an address reused by a later allocation would still resolve
    // to the dead object — the registry only ever grew. It also gives the
    // symmetric buffers the same [open, close) treatment MRs, QPs and UARs
    // already get, so entity_close_accounted covers them too.
    //
    // opener is MemObject's type_id because that is the record that opened it;
    // a record id rather than a new enum, so nothing has to be kept in sync.
    void on_symmetric_free(std::uint64_t base) { close_object(base); }

    // The shared body: emit the close for whatever object is registered at this
    // exact base. Silent when nothing is — a free of memory allocated before we
    // attached is not a defect, and inventing a close for it would manufacture
    // exactly the unmatched row entity_close_accounted exists to detect.
    void close_object(std::uint64_t base) {
        const auto object = m_allocs.object_at(base);
        if (object == core::kNullHandle)
            return;
        auto& rec = recorder();
        rec.record(rec.now_instant(),
                   core::records::EntityClose{
                       {}, {}, object, core::records::MemObject::pcomm_type_id});
    }

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
            const auto alloc = m_allocs.find(a.address);
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
