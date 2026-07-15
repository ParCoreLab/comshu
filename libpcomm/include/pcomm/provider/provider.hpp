#pragma once
// L3 boundary — every provider is a .so exporting a C factory; the control
// plane (Tracer/ProviderRegistry) discovers and loads them at runtime.
// Abstract Factory + Registry; pcomm_abi_version() guards ABI drift.

#include <cstdint>
#include <string_view>

#include "pcomm/core/ids.hpp"
#include "pcomm/rec/recorder.hpp"

namespace pcomm::provider {

inline constexpr std::uint32_t kAbiVersion = 1;

struct ProviderInfo {
    std::string_view   name;
    core::RecordTypeId id_range_lo{0};
    core::RecordTypeId id_range_hi{0};
};

// Run configuration relevant to a provider (subset of the launcher config,
// passed through env by the pcomm launcher as PCOMM_* variables).
struct ProviderConfig {
    std::string_view output_dir;
    int  rank{0};
    bool verbose{false};
};

class IProvider {
public:
    virtual ~IProvider() = default;
    virtual ProviderInfo info() const = 0;
    virtual bool init(const ProviderConfig&, rec::Recorder<>&) = 0;
    virtual void shutdown() = 0;
};

}  // namespace pcomm::provider

// Each provider .so exports exactly these two symbols:
//   extern "C" pcomm::provider::IProvider* pcomm_create_provider();
//   extern "C" std::uint32_t               pcomm_abi_version();
#define PCOMM_EXPORT_PROVIDER(ProviderClass)                          \
    extern "C" ::pcomm::provider::IProvider* pcomm_create_provider() {\
        return new ProviderClass();                                   \
    }                                                                 \
    extern "C" std::uint32_t pcomm_abi_version() {                    \
        return ::pcomm::provider::kAbiVersion;                        \
    }
