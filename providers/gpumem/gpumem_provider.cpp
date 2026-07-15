// GPU memory-access provider (Snoopie engine). Skeleton: wires the
// attribution pipeline to the recorder; the NVBit backend port
// (snoopie/src/mem_multigpu -> gpu-backends/nvbit) plugs in via IGpuBackend.

#include <memory>

#include "pcomm/attr/pipeline.hpp"
#include "pcomm/gpu/backend.hpp"
#include "pcomm/provider/provider.hpp"

namespace pcomm::providers {

// Keeps accesses that cross devices (the communication we care about).
class RemoteFilter final : public attr::IFilter {
public:
    bool keep(const attr::RawAccess& a) const override {
        return a.run_device != a.mem_device;
    }
};

class GpumemProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"gpumem", core::id_range::gpumem_lo, core::id_range::gpumem_hi};
    }

    bool init(const provider::ProviderConfig&, rec::Recorder<>& rec) override {
        m_pipeline = std::make_unique<attr::AttributionPipeline>(rec);
        m_pipeline->add_filter(std::make_unique<RemoteFilter>());
        // TODO(phase 1): create IGpuBackend (NVBit), subscribe the pipeline to
        // kernel-launch events, add Device/Object/SourceLine attributors per
        // config, and drive process() from the collector drain loop.
        return true;
    }

    void shutdown() override {
        if (m_backend) m_backend->shutdown();
    }

private:
    std::unique_ptr<attr::AttributionPipeline> m_pipeline;
    std::unique_ptr<gpu::IGpuBackend> m_backend;
};

}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::GpumemProvider)
