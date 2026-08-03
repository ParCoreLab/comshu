// thread_flush_smoke — cross-thread flush. N worker threads each record many
// records and exit WITHOUT draining their thread-local buffers; the main
// thread then flushes everything via ThreadBufferRegistry::drain_all() and we
// verify every record reached the sink. Before the registry, all of these
// records were lost (only the stopping thread's buffers drained).

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/recorder.hpp"
#include "pcomm/rec/sink.hpp"
#include "pcomm/rec/stream_header.hpp"
#include "pcomm/rec/thread_buffer_registry.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"
#include "pcomm/tracer.hpp"

namespace fs = std::filesystem;
using namespace pcomm;
using core::records::MemObject;

int main() {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;  // < buffer_capacity (4096): every record
                                      // relies entirely on the central flush.

    fs::path dir = fs::temp_directory_path() / "pcomm-thread-flush";
    fs::remove_all(dir);

    // Records go into engine-owned arenas (engine_abi.h), not into a sink this
    // test holds — so the engine is what has to be started, and the sink it
    // owns is the one that ends up on disk. Constructing a local FileSink here
    // and expecting records in it, as this test used to, silently observes
    // nothing: the arenas belong to the engine either way.
    Config cfg;
    cfg.output_dir = dir;
    cfg.rank = 0;
    Tracer::instance().start(cfg);
    auto& recorder = Tracer::instance().recorder();

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&recorder, t] {
            for (int i = 0; i < kPerThread; ++i) {
                recorder.record(MemObject{{},
                                          {},
                                          static_cast<core::Handle>(t * kPerThread + i + 1),
                                          core::kNullHandle,
                                          {static_cast<std::uint64_t>(i), 64},
                                          static_cast<core::DeviceId>(t)});
            }
            // Thread exits WITHOUT draining — buffers stay in its (leaked)
            // thread-local store, tracked only by the global registry.
        });
    }
    for (auto& w : workers)
        w.join();

    // Central flush from the main thread (which never recorded).
    std::printf("registry buffers before drain = %zu\n",
                rec::ThreadBufferRegistry::instance().size());
    rec::ThreadBufferRegistry::instance().drain_all();
    Tracer::instance().finalize();

    // Tracer::start writes into <output_dir>/rank-<N>/.
    auto path = dir / "rank-0" / (std::to_string(core::RecordTrait<MemObject>::type_id) + ".pcs");
    if (!fs::exists(path)) {
        std::printf("MISSING stream %s\n", path.c_str());
        return 2;
    }
    // Every stream opens with a 32-byte StreamHeader; the records follow it.
    auto bytes = fs::file_size(path) - sizeof(rec::StreamHeader);
    auto rec_size = sizeof(rec::Stamped<MemObject>);
    auto count = bytes / rec_size;

    std::printf("threads=%d per_thread=%d expected=%d got=%zu\n", kThreads, kPerThread,
                kThreads * kPerThread, static_cast<std::size_t>(count));
    assert(bytes % rec_size == 0 && "stream not a whole number of records");
    assert(count == static_cast<std::size_t>(kThreads) * kPerThread &&
           "cross-thread flush dropped records");
    std::puts("thread_flush_smoke OK");
    return 0;
}
