// fork_smoke — the target fork()s after tracing has started. The child must
// record into its own rank dir without flushing/corrupting the parent's
// stream. Validates the pthread_atfork child handler: release-not-destroy the
// parent's sink+recorder, drop tracked buffers unflushed, re-init the child
// into a pid-named dir.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"
#include "pcomm/tracer.hpp"

namespace fs = std::filesystem;
using namespace pcomm;
using core::records::MemObject;

static void record_n(int n, std::uint16_t dev) {
    auto& r = Tracer::instance().recorder();
    for (int i = 0; i < n; ++i)
        r.record(MemObject{{}, {}, static_cast<core::Handle>(i + 1), core::kNullHandle,
                           {static_cast<std::uint64_t>(i), 64}, dev});
}

static std::size_t stream_count(const fs::path& dir) {
    auto path = dir / (std::to_string(core::RecordTrait<MemObject>::type_id) + ".pcs");
    if (!fs::exists(path)) return 0;
    return fs::file_size(path) / sizeof(rec::Stamped<MemObject>);
}

int main() {
    constexpr int kParentPre = 100, kChild = 50, kParentPost = 30;

    fs::path out = fs::temp_directory_path() / ("pcomm-fork-" + std::to_string(::getpid()));
    fs::remove_all(out);

    Config cfg;
    cfg.output_dir = out;
    cfg.rank = 0;
    Tracer::instance().start(cfg);
    record_n(kParentPre, 0);  // buffered in the main-thread buffer at fork time

    pid_t pid = fork();
    if (pid == 0) {  // child: records must land in rank-<pid>, not the parent's
        record_n(kChild, 1);
        Tracer::instance().stop();
        _exit(0);
    }
    assert(pid > 0 && "fork failed");
    int status = 0;
    waitpid(pid, &status, 0);

    record_n(kParentPost, 0);  // parent keeps recording into its own buffer
    Tracer::instance().stop();

    auto parent_n = stream_count(out / "rank-0");
    auto child_n = stream_count(out / ("rank-" + std::to_string(pid)));

    std::printf("child exit=%d; parent rank-0=%zu (expect %d), child rank-%d=%zu (expect %d)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1, parent_n,
                kParentPre + kParentPost, (int)pid, child_n, kChild);

    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && "child crashed");
    assert(child_n == static_cast<std::size_t>(kChild) && "child records lost/misrouted");
    assert(parent_n == static_cast<std::size_t>(kParentPre + kParentPost) &&
           "parent stream corrupted or truncated by the fork child");
    std::puts("fork_smoke OK");
    return 0;
}
