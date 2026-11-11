#pragma once
// L2 — attribution pipeline interfaces (GPU mem-access side). Promoted from
// snoopie-sanitized's interfaces.cuh, vendor-free: no Sanitizer_*/CUDA types.
// Providers translate vendor events into these neutral forms.
//
// Patterns: Observer (runtime events), Strategy (IFilter), Chain of
// Responsibility (attributor chain) — see ../ARCHITECTURE.md §5.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "comshu/core/ids.hpp"
#include "comshu/core/records.hpp"
#include "comshu/core/vocab.hpp"
#include "comshu/rec/recorder.hpp"

namespace comshu::attr {

// Neutral per-access sample (was snoopie's MemoryAccess / Entry, sans vendor
// types). The device-side collector fills these; the host drains them.
// `pc` is a producer-defined identity, already handle-ready: producers with
// dense instruction indices pass index+1; producers with raw PCs intern them
// (InternDomain::Pc) before filling RawAccess. The pipeline copies it as-is,
// so it joins directly against SourceLine/Artifact rows using the same scheme.
// mem_device: producers that can resolve ownership themselves set it; others
// leave kUnresolvedDevice and the consumer resolves via its allocation
// registry. The gpumem backend takes the second route for everything,
// including NVSHMEM's symmetric heap — one lookup, one place to be wrong.
inline constexpr core::DeviceId kUnresolvedDevice = 0xFFFF;

struct RawAccess {
    std::uint64_t  address{0};
    std::uint64_t  pc{0};
    std::uint32_t  size{0};
    std::uint32_t  thread{0};
    core::MemOp    op{core::MemOp::Load};
    core::MemSpace space{core::MemSpace::Global};
    core::DeviceId run_device{0};
    core::DeviceId mem_device{kUnresolvedDevice};

    // When this access *might* have happened, as host ticks — the pipeline
    // emits it as a Temporal::window, not an Instant.
    //
    // A device-side collector has no clock the host shares: reading the GPU's
    // globaltimer on every instrumented instruction would sit on the hot path
    // of the very thing being measured. So a producer bounds the access
    // instead, and the bound is the honest claim:
    //
    //   ticks_lo — the earliest it could have happened. For NVBit, when the
    //              kernel that issued it was launched. 0 = unknown (open
    //              below); the pipeline still emits a window, never invents a
    //              lower bound.
    //   ticks_hi — the latest. For NVBit, when the host drained it from the
    //              channel: the access provably preceded its own dequeue.
    //
    // Producers that leave both 0 get a timeless record rather than a
    // fabricated instant. See
    // plans/PLAN-20260803-1021-gpumem-window-timestamps-*.md for why the
    // alternatives (previous-drain time; per-access globaltimer) were rejected.
    core::Ticks ticks_lo{0};
    core::Ticks ticks_hi{0};

    // Which kernel launch issued it, for the reader to join against a recorded
    // kernel span — see GpuAccess::launch in core/records.hpp for why this is a
    // key rather than a tighter pair of ticks. 0 = unknown.
    std::uint64_t launch{0};
};

// Host-side runtime happenings the pipeline reacts to (kernel launch/exit,
// allocation, process events). Grows as the Snoopie port needs it.
struct RuntimeEvent {
    enum class Kind : std::uint8_t {
        KernelLaunch,
        KernelExit,
        Alloc,
        Free,
        ProcessExit,
    } kind{Kind::KernelLaunch};
    core::DeviceId device{0};
    core::MemRange range{};  // Alloc/Free
    core::Handle   name{0};  // interned kernel/symbol name
};

class IEventObserver {
public:
    virtual ~IEventObserver() = default;
    virtual void on_event(const RuntimeEvent&) = 0;
};

class IEventObservable {
public:
    virtual ~IEventObservable() = default;
    virtual void subscribe(IEventObserver*) = 0;
    virtual void notify(const RuntimeEvent&) = 0;
};

// Host-side view of the device collector; one impl per GPU backend.
class ICollector {
public:
    virtual ~ICollector() = default;
    virtual std::span<const RawAccess> drain() = 0;
};

// Host-side predicate deciding which accesses survive (e.g. RemoteFilter keeps
// run_device != mem_device). Backends may mirror it device-side for on-device
// filtering; the contract here is the host fallback.
class IFilter {
public:
    virtual ~IFilter() = default;
    virtual bool keep(const RawAccess&) const = 0;
};

// Enriches an access into Entities/Artifacts/Edges written to the recorder.
// Concrete phase-1 attributors: Device, Object, SourceLine, CudaIpc.
class IAttributor : public IEventObserver {
public:
    // Fills the inline links on the GpuAccess record and/or emits side records.
    virtual void attribute(const RawAccess&, core::records::GpuAccess& out,
                           rec::Recorder<>& rec) = 0;
    void         on_event(const RuntimeEvent&) override {}
};

// filter -> attributor chain -> recorder. Each attributor is independent and
// individually disable-able (absent from the chain when the user disables it).
class AttributionPipeline final : public IEventObserver {
public:
    explicit AttributionPipeline(rec::Recorder<>& rec) : m_rec(&rec) {}

    void add_filter(std::unique_ptr<IFilter> f) { m_filters.push_back(std::move(f)); }
    void add_attributor(std::unique_ptr<IAttributor> a) { m_chain.push_back(std::move(a)); }

    void on_event(const RuntimeEvent& ev) override {
        for (auto& a : m_chain)
            a->on_event(ev);
    }

    void process(std::span<const RawAccess> accesses);

private:
    rec::Recorder<>*                          m_rec;
    std::vector<std::unique_ptr<IFilter>>     m_filters;
    std::vector<std::unique_ptr<IAttributor>> m_chain;
};

}  // namespace comshu::attr
