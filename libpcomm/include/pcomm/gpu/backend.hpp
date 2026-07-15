#pragma once
// L2 — GPU HAL (Bridge): one stable interface, many vendor impls. Adding AMD
// means one new IGpuBackend + device collector, zero changes above this line.
// Vendor headers are FORBIDDEN here; NvbitBackend/SanitizerBackend/RocmBackend
// live in gpu-backends/ and include them there.

#include <cstdint>
#include <memory>
#include <string_view>

#include "pcomm/attr/interfaces.hpp"
#include "pcomm/core/ids.hpp"

namespace pcomm::gpu {

struct BackendInfo {
    std::string_view vendor;      // "nvidia", "amd"
    std::string_view mechanism;   // "nvbit", "compute-sanitizer", "rocm"
    bool device_side_filtering{false};
};

// Opaque, vendor-neutral handles; backends translate internally.
using StreamHandle = std::uint64_t;

struct KernelLaunch {
    core::DeviceId device{0};
    core::Handle   name{0};     // interned kernel name
    StreamHandle   stream{0};
};

// Paired sample for GPU<->CPU clock alignment (pcomm-read fits the mapping).
struct ClockSample {
    core::Ticks gpu_ticks{0};
    core::Ticks cpu_ticks{0};
};

class IGpuBackend {
public:
    virtual ~IGpuBackend() = default;
    virtual BackendInfo info() const = 0;
    virtual void instrument(const KernelLaunch&) = 0;   // install device-side collector
    virtual std::unique_ptr<attr::ICollector> make_collector(StreamHandle) = 0;
    virtual ClockSample clock_sample() = 0;
    virtual void shutdown() = 0;
};

}  // namespace pcomm::gpu
