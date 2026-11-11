// The first full arena must not stall the thread that filled it.
//
// The first flush used to run the engine's one-time emissions — GpuDevice (a
// CUDA driver query) and Invocation (argv, env, and every mapped image) —
// inline on the recording thread. Measured on du04's send-queue watcher at
// 2.1-2.4 ms against microseconds for every later batch; on a doorbell repeater
// that is an application doorbell held for 2 ms. They now run on an engine
// thread, joined at finalize.
//
// Asserted: the Invocation row is still written (moving it must not lose it).
// Printed, not asserted: the slowest record() calls, i.e. the flushes. A
// latency threshold in a unit test is a flaky test; the numbers are for the
// person comparing before and after.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "comshu/core/records.hpp"
#include "comshu/rec/stream_header.hpp"
#include "comshu/tracer.hpp"

namespace fs = std::filesystem;
using namespace comshu;
using core::records::MemObject;

int main() {
    fs::path dir = fs::temp_directory_path() / "comshu-first-flush";
    fs::remove_all(dir);
    Config cfg;
    cfg.output_dir = dir;
    cfg.rank = 0;
    Tracer::instance().start(cfg);
    auto& recorder = Tracer::instance().recorder();

    constexpr int       kRecords = 3 * 4096 + 16;  // three full arenas
    std::vector<double> us(kRecords);
    for (int i = 0; i < kRecords; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        recorder.record(MemObject{{},
                                  {},
                                  static_cast<core::Handle>(i + 1),
                                  core::kNullHandle,
                                  {static_cast<std::uint64_t>(i), 64},
                                  core::kUnknownDevice,
                                  core::AttrStatus::HostNotCuda});
        us[i] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                    .count();
    }
    std::vector<int> idx(kRecords);
    for (int i = 0; i < kRecords; ++i)
        idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin() + 3, idx.end(),
                      [&](int a, int b) { return us[a] > us[b]; });
    std::printf("slowest record() calls (the flushes): ");
    for (int k = 0; k < 3; ++k)
        std::printf("#%d %.1f us  ", idx[k], us[idx[k]]);
    std::printf("\n");

    Tracer::instance().finalize();

    const auto inv = dir / "rank-0" /
                     (std::to_string(core::RecordTrait<core::records::Invocation>::type_id) + ".pcs");
    if (!fs::exists(inv) || fs::file_size(inv) <= sizeof(rec::StreamHeader)) {
        std::printf("FAIL: no Invocation row — moving the one-time emissions lost it\n");
        return 1;
    }
    std::puts("first_flush_latency OK");
    return 0;
}
