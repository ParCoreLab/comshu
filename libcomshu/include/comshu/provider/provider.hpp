#pragma once
// L3 boundary — every provider is a .so exporting a C factory; the control
// plane (Tracer/ProviderRegistry) discovers and loads them at runtime.
// Abstract Factory + Registry; comshu_abi_version() guards ABI drift.

#include <cstdint>
#include <string_view>

#include "comshu/core/ids.hpp"
#include "comshu/rec/recorder.hpp"

namespace comshu::provider {

inline constexpr std::uint32_t kAbiVersion = 1;

struct ProviderInfo {
    std::string_view   name;
    core::RecordTypeId id_range_lo{0};
    core::RecordTypeId id_range_hi{0};
};

// Run configuration relevant to a provider (subset of the launcher config,
// passed through env by the comshu launcher as COMSHU_* variables).
struct ProviderConfig {
    std::string_view output_dir;
    int              rank{0};
    bool             verbose{false};
};

class IProvider {
public:
    virtual ~IProvider() = default;
    virtual ProviderInfo info() const = 0;
    virtual bool         init(const ProviderConfig&, rec::Recorder<>&) = 0;
    virtual void         shutdown() = 0;
};

}  // namespace comshu::provider

// Each provider .so exports exactly these two symbols:
//   extern "C" comshu::provider::IProvider* comshu_create_provider();
//   extern "C" std::uint32_t               comshu_abi_version();
#define COMSHU_EXPORT_PROVIDER(ProviderClass)                                                       \
    extern "C" ::comshu::provider::IProvider* comshu_create_provider() {                             \
        return new ProviderClass();                                                                \
    }                                                                                              \
    extern "C" std::uint32_t comshu_abi_version() {                                                 \
        return ::comshu::provider::kAbiVersion;                                                     \
    }

// Absolute path of the loaded libcomshu_engine.so, or nullptr if it can't be
// determined. The launcher needs this to put the engine in LD_PRELOAD — see the
// definition in libcomshu/src/engine.cpp for why merely linking it is not enough.
extern "C" const char* comshu_engine_so_path();
