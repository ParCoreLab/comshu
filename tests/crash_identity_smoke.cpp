// crash_identity_smoke — the target is KILLED, and the capture must still be
// able to say which machine and which clock it came from.
//
// WHY THIS EXISTS. Nothing in the engine flushes on an abnormal exit: there is
// no signal handler anywhere in libpcomm, so a process killed by a signal keeps
// only what happened to be on disk already. That failure is INVERTED, which is
// what makes it dangerous rather than merely unfortunate: a high-volume stream
// survives because it filled its buffers and drained incrementally, while a
// stream of one or two rows never drained at all.
//
// Measured on Quantum ESPRESSO (tests/hpc/apps/03_quantumespresso), whose pw.x
// reaches JOB DONE and then faults in its own teardown: 18,700,773 ib_post_send
// rows survived, ib_modify_qp kept 94, node_identity covered 2 of 4 ranks, and
// 84% of sends therefore landed on dst_rank = -1 with comm_edges reporting 6
// edges where 12 existed. The broken capture was the BIGGER one — every
// heuristic a human or a check would use to notice it points the wrong way.
//
// start() now drains and flushes immediately after emitting the two startup
// facts. This test is the reason that line cannot be deleted by someone who
// reads it as a redundant flush: it kills a child mid-capture, with records
// deliberately left unflushed, and requires those two streams to be readable
// anyway.
//
// WHAT THIS DOES NOT CLAIM. It is not crash-safety. Everything recorded after
// start() is still lost on a signal — that is todos.txt item 15 and it is a
// signal handler, not a flush. What is asserted here is the floor: a killed
// capture can still be placed on a node and on a clock.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pcomm/core/records.hpp"
#include "pcomm/rec/thread_local_buffer.hpp"
#include "pcomm/tracer.hpp"

namespace fs = std::filesystem;
using namespace pcomm;
using core::records::ClockAnchor;
using core::records::MemObject;
using core::records::NodeIdentity;

// Rows in a stream file, past the 32-byte StreamHeader every one opens with.
template<class R>
static std::size_t rows(const fs::path& dir) {
    auto path = dir / (std::to_string(core::RecordTrait<R>::type_id) + ".pcs");
    if (!fs::exists(path))
        return 0;
    const auto sz = fs::file_size(path);
    if (sz < sizeof(rec::StreamHeader))
        return 0;
    return (sz - sizeof(rec::StreamHeader)) / sizeof(rec::Stamped<R>);
}

int main() {
    const fs::path out = fs::temp_directory_path() / ("pcomm-crash-" + std::to_string(::getpid()));
    fs::remove_all(out);

    const pid_t pid = ::fork();
    assert(pid >= 0 && "fork failed");

    if (pid == 0) {
        // The child brings the tracer up, records into a thread buffer, and is
        // then killed without ever reaching stop(), finalize(), or the atexit
        // backstop. SIGKILL rather than SIGSEGV on purpose: it cannot be caught,
        // so this test keeps meaning exactly what it means today even after item
        // 15 installs a handler for the catchable signals. It measures the
        // FLUSH, not the handler.
        Config cfg;
        cfg.output_dir = out;
        cfg.rank = 0;
        Tracer::instance().start(cfg);

        // Deliberately left in the buffer. These must NOT survive — if they do,
        // the flush is happening somewhere this test does not know about and the
        // assertions below would be measuring the wrong thing.
        auto& r = Tracer::instance().recorder();
        for (int i = 0; i < 8; ++i)
            r.record(MemObject{
                {}, {}, static_cast<core::Handle>(i + 1), core::kNullHandle, {0x1000, 64}, 0});

        ::raise(SIGKILL);
        _exit(99);  // unreachable; if it runs, the kill did not happen
    }

    int status = 0;
    assert(::waitpid(pid, &status, 0) == pid);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        std::fprintf(stderr, "crash_identity_smoke: child did not die by SIGKILL (status %d)\n",
                     status);
        return 1;
    }

    const fs::path dir = out / "rank-0";
    int            fail = 0;

    // THE CLAIM. Both startup facts are on disk, from a process that was killed
    // and never finalized anything.
    const auto nodes = rows<NodeIdentity>(dir);
    const auto anchors = rows<ClockAnchor>(dir);
    if (nodes != 1) {
        std::fprintf(stderr,
                     "crash_identity_smoke: node_identity has %zu rows, expected 1 — a killed\n"
                     "  capture cannot say which machine it came from, and every rank-space\n"
                     "  join in the reader is anchored on this row\n",
                     nodes);
        fail = 1;
    }
    if (anchors != 1) {
        std::fprintf(stderr,
                     "crash_identity_smoke: clock_anchor has %zu rows, expected 1 — a killed\n"
                     "  capture cannot be placed on a wall clock at all\n",
                     anchors);
        fail = 1;
    }

    // THE CONTROL, and it is what stops this test passing for the wrong reason.
    // If ordinary records survived too, then something is flushing on every
    // record and the two assertions above would pass whatever start() does.
    const auto objects = rows<MemObject>(dir);
    if (objects != 0) {
        std::fprintf(stderr,
                     "crash_identity_smoke: %zu mem_object rows survived a SIGKILL, expected 0.\n"
                     "  The test is not measuring the startup flush any more — something is\n"
                     "  flushing per record, and these assertions would pass regardless.\n",
                     objects);
        fail = 1;
    }

    fs::remove_all(out);
    if (fail == 0)
        std::printf("crash_identity_smoke: ok — identity survives a kill, buffered records do not\n");
    return fail;
}
