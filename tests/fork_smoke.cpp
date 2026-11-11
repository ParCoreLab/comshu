// fork_smoke — the target fork()s after tracing has started. The child must
// record into its own rank dir without flushing/corrupting the parent's
// stream. Validates the pthread_atfork child handler: release-not-destroy the
// parent's sink+recorder, drop tracked buffers unflushed, re-init the child
// into a pid-named dir.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "comshu/core/records.hpp"
#include "comshu/rec/thread_local_buffer.hpp"
#include "comshu/tracer.hpp"

namespace fs = std::filesystem;
using namespace comshu;
using core::records::MemObject;

static void record_n(int n, std::uint16_t dev) {
    auto& r = Tracer::instance().recorder();
    for (int i = 0; i < n; ++i)
        r.record(MemObject{{},
                           {},
                           static_cast<core::Handle>(i + 1),
                           core::kNullHandle,
                           {static_cast<std::uint64_t>(i), 64},
                           dev});
}

static std::size_t stream_count(const fs::path& dir) {
    auto path = dir / (std::to_string(core::RecordTrait<MemObject>::type_id) + ".pcs");
    if (!fs::exists(path))
        return 0;
    // Past the 32-byte StreamHeader every stream file opens with.
    return (fs::file_size(path) - sizeof(rec::StreamHeader)) / sizeof(rec::Stamped<MemObject>);
}

// How many records in this stream carry `dev`. Counts alone cannot tell whose
// records these are: parent and child write the same *number* of MemObjects in
// some failure modes, and a fix that re-registered the parent's arena without
// clearing it would put the parent's 100 into the child's file and still count
// right. Parent records use device 0, child records device 1.
static std::size_t stream_count_with_device(const fs::path& dir, std::uint16_t dev) {
    auto path = dir / (std::to_string(core::RecordTrait<MemObject>::type_id) + ".pcs");
    if (!fs::exists(path))
        return 0;
    std::ifstream in(path, std::ios::binary);
    in.seekg(sizeof(rec::StreamHeader));
    std::size_t             n = 0;
    rec::Stamped<MemObject> s{};
    while (in.read(reinterpret_cast<char*>(&s), sizeof s))
        if (s.payload.device == dev)
            ++n;
    return n;
}

int main() {
    constexpr int kParentPre = 100, kChild = 50, kParentPost = 30;

    fs::path out = fs::temp_directory_path() / ("comshu-fork-" + std::to_string(::getpid()));
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
                WIFEXITED(status) ? WEXITSTATUS(status) : -1, parent_n, kParentPre + kParentPost,
                (int)pid, child_n, kChild);

    // Reported rather than asserted, and all of them before exiting: one run
    // then shows the whole picture instead of stopping at the first bad number,
    // and a non-zero exit is something ctest's WILL_FAIL can express while the
    // SIGABRT from assert() is not.
    int  bad = 0;
    auto check = [&bad](bool ok, const char* what) {
        if (!ok) {
            std::printf("  FAIL: %s\n", what);
            ++bad;
        }
    };
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child crashed");
    check(child_n == static_cast<std::size_t>(kChild), "child records lost/misrouted");
    check(parent_n == static_cast<std::size_t>(kParentPre + kParentPost),
          "parent stream corrupted or truncated by the fork child");

    // Whose records are they? Counts alone would accept a child stream full of
    // the parent's pre-fork buffer — which is exactly what re-registering the
    // arenas *without* clearing them would produce.
    auto child_own = stream_count_with_device(out / ("rank-" + std::to_string(pid)), 1);
    auto parent_own = stream_count_with_device(out / "rank-0", 0);
    std::printf("  child rank-%d holds %zu child-tagged records (expect %d); "
                "parent rank-0 holds %zu parent-tagged (expect %d)\n",
                (int)pid, child_own, kChild, parent_own, kParentPre + kParentPost);
    check(child_own == static_cast<std::size_t>(kChild),
          "child stream does not hold the child's own records");
    check(parent_own == static_cast<std::size_t>(kParentPre + kParentPost),
          "parent stream does not hold the parent's own records");

    if (bad) {
        std::printf("fork_smoke: %d check(s) failed\n", bad);
        return 1;
    }
    std::puts("fork_smoke OK");
    return 0;
}
