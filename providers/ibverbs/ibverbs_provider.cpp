// ibverbs proof provider. Skeleton: provider shell only. The port brings
// SigmaTrace's Ibtrace event source over the pcomm::hook engine
// (DEFINE_PRELOAD_HOOK/DEFINE_OBJECT_OPS_HOOK for ibv_reg_mr, ibv_post_send,
// ibv_create_qp, poll_cq, ...), with records in the 100-range.

#include "pcomm/provider/provider.hpp"

namespace pcomm::providers {

class IbverbsProvider final : public provider::IProvider {
public:
    provider::ProviderInfo info() const override {
        return {"ibverbs", core::id_range::ibverbs_lo, core::id_range::ibverbs_hi};
    }

    bool init(const provider::ProviderConfig&, rec::Recorder<>&) override {
        // TODO(phase 1): install the IbverbsEventSource hook set.
        return true;
    }

    void shutdown() override {}
};

}  // namespace pcomm::providers

PCOMM_EXPORT_PROVIDER(pcomm::providers::IbverbsProvider)
