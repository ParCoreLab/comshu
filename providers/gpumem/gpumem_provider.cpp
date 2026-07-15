// GPU memory-access provider shell. The heavy lifting happens in the NVBit
// backend (gpu-backends/nvbit/nvbit_backend.cu), whose nvbit_at_init runs on
// tool load and constructs the gpumem::Engine; this IProvider exists for the
// control plane / provider registry (info, id ranges, explicit shutdown).

#include "pcomm/provider/provider.hpp"

namespace pcomm::providers {

class GpumemProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"gpumem", core::id_range::gpumem_lo, core::id_range::gpumem_hi};
    }

    bool init(const provider::ProviderConfig&, rec::Recorder<>&) override {
        // Engine lifecycle is driven by the NVBit callbacks (nvbit_at_init /
        // nvbit_at_term), which fire on .so load — nothing to do here yet.
        return true;
    }

    void shutdown() override {}
};

}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::GpumemProvider)
