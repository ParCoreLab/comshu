// arena_drain_race — records must reach disk exactly once, whole, while other
// threads drain the arena they are being appended to (CR-08).
//
// finalize() and the fatal-signal handler drain every thread's arena from a
// thread that does not own it. Before CR-08 that drain read `count` and zeroed
// it with no synchronisation against the owner's `++count`, so a record could
// be submitted half-written, or submitted twice when the owner wrote back a
// count the drain had just reset. Here several threads record numbered
// MemObjects as fast as they can while the main thread drains every registered
// arena in a loop; afterwards every (thread, sequence) pair must appear exactly
// once with its payload intact.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <unistd.h>

#include "comshu/core/records.hpp"
#include "comshu/rec/thread_buffer_registry.hpp"
#include "comshu/rec/thread_local_buffer.hpp"
#include "comshu/tracer.hpp"

namespace fs = std::filesystem;
using namespace comshu;
using core::records::MemObject;

int main() {
    constexpr int kThreads = 4, kPerThread = 300000;
    const fs::path out = fs::temp_directory_path() / ("comshu-drainrace-" + std::to_string(::getpid()));
    fs::remove_all(out);

    Config cfg;
    cfg.output_dir = out;
    cfg.rank = 0;
    Tracer::instance().start(cfg);

    std::atomic<int>  running{kThreads};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([t, &running] {
            auto& r = Tracer::instance().recorder();
            for (int i = 0; i < kPerThread; ++i)
                // handle = sequence, device = thread, extent mirrors both so a
                // torn record (half old, half new) is detectable.
                r.record(MemObject{{},
                                   {},
                                   static_cast<core::Handle>(i + 1),
                                   core::kNullHandle,
                                   {static_cast<std::uint64_t>(i) * 64 + t, 64},
                                   static_cast<std::uint16_t>(t)});
            Tracer::instance().recorder().drain_thread();
            running.fetch_sub(1);
        });
    // The cross-thread drain finalize() and the signal handler perform, as
    // often as possible while the owners append.
    std::uint64_t drains = 0;
    while (running.load() > 0) {
        rec::ThreadBufferRegistry::instance().drain_all();
        ++drains;
    }
    for (auto& th : ts)
        th.join();
    Tracer::instance().stop();

    const auto path = out / "rank-0" / (std::to_string(core::RecordTrait<MemObject>::type_id) + ".pcs");
    std::ifstream in(path, std::ios::binary);
    in.seekg(sizeof(rec::StreamHeader));
    std::vector<std::vector<std::uint8_t>> seen(kThreads, std::vector<std::uint8_t>(kPerThread, 0));
    std::uint64_t total = 0, torn = 0, dup = 0, alien = 0;
    rec::Stamped<MemObject> s{};
    while (in.read(reinterpret_cast<char*>(&s), sizeof s)) {
        ++total;
        const auto t = s.payload.device;
        const auto i = static_cast<std::int64_t>(s.payload.id) - 1;
        if (t >= kThreads || i < 0 || i >= kPerThread) {
            ++alien;
            continue;
        }
        if (s.payload.range.base != static_cast<std::uint64_t>(i) * 64 + t ||
            s.payload.range.size != 64) {
            ++torn;
            continue;
        }
        if (seen[t][i]++)
            ++dup;
    }
    std::uint64_t missing = 0;
    for (auto& v : seen)
        for (auto c : v)
            missing += c == 0;
    fs::remove_all(out);

    std::printf("arena_drain_race: %llu drains; %llu records (expect %d): dup=%llu torn=%llu "
                "alien=%llu missing=%llu\n",
                (unsigned long long)drains, (unsigned long long)total, kThreads * kPerThread,
                (unsigned long long)dup, (unsigned long long)torn, (unsigned long long)alien,
                (unsigned long long)missing);
    const bool ok = total == std::uint64_t{kThreads} * kPerThread && !dup && !torn && !alien &&
                    !missing;
    std::puts(ok ? "arena_drain_race: ok" : "arena_drain_race: FAIL");
    return ok ? 0 : 1;
}
