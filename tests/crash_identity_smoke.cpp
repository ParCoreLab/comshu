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
#include "pcomm/rec/intern_table.hpp"
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

// Runs one child that starts a capture, records `n` MemObjects into a thread
// buffer WITHOUT flushing, and then dies by `sig`. Returns the signal it
// actually died by, so the caller can check that pcomm did not change how the
// program appears to fail.
static int run_dying_child(const fs::path& out, int sig, int n) {
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

        // Deliberately left in the buffer: nothing here calls stop(), and the
        // process never reaches the atexit backstop.
        auto& r = Tracer::instance().recorder();
        // Intern a real string, so the record below carries a Handle that is
        // meaningless without the dictionary. That is what makes the intern arm
        // of the assertions a genuine test of the handler's risky half rather
        // than a check that an unconditional file exists: with nothing interned
        // emit_intern_records writes no blob at all, and correctly so.
        const core::Handle name = r.intern(rec::InternDomain::String, "crash-arm");
        for (int i = 0; i < n; ++i)
            r.record(MemObject{
                {}, {}, static_cast<core::Handle>(i + 1), name, {0x1000, 64}, 0});

        ::raise(sig);
        _exit(99);  // unreachable unless the signal was swallowed entirely
    }
    int status = 0;
    assert(::waitpid(pid, &status, 0) == pid);
    return WIFSIGNALED(status) ? WTERMSIG(status) : -status;
}

int main() {
    const fs::path base = fs::temp_directory_path() / ("pcomm-crash-" + std::to_string(::getpid()));
    fs::remove_all(base);
    int fail = 0;

    // ── ARM 1: SIGKILL, which no handler can catch ──────────────────────────
    //
    // This measures the STARTUP FLUSH and nothing else, and it keeps meaning
    // that however the signal handler below evolves. Uncatchable on purpose.
    const fs::path out = base / "kill";
    const int      died = run_dying_child(out, SIGKILL, 8);
    if (died != SIGKILL) {
        std::fprintf(stderr, "crash_identity_smoke: child did not die by SIGKILL (got %d)\n", died);
        return 1;
    }

    const fs::path dir = out / "rank-0";

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

    // ── ARM 2: SIGSEGV, which the handler DOES catch ────────────────────────
    //
    // The difference between the two arms is the whole value of the handler:
    // same workload, same records left unflushed, and the only variable is
    // whether pcomm got a chance to run. Under SIGKILL the eight MemObjects are
    // gone; here they must all be on disk.
    const fs::path out2 = base / "segv";
    const int      died2 = run_dying_child(out2, SIGSEGV, 8);

    // FIRST, and it matters more than the recovery: the process still died by
    // SIGSEGV. A profiler must not change how the program it is watching appears
    // to fail — a handler that swallowed the signal, or exited with a status of
    // its own, would hide the very crash someone is trying to debug and would
    // lie to the batch scheduler about what happened.
    if (died2 != SIGSEGV) {
        std::fprintf(stderr,
                     "crash_identity_smoke: child died by %d, not SIGSEGV — the handler changed\n"
                     "  how the program fails, which is worse than losing the capture\n",
                     died2);
        fail = 1;
    }

    const fs::path dir2 = out2 / "rank-0";
    const auto     recovered = rows<MemObject>(dir2);
    if (recovered != 8) {
        std::fprintf(stderr,
                     "crash_identity_smoke: %zu of 8 mem_object rows recovered from a SIGSEGV,\n"
                     "  expected 8 — the fatal-signal handler did not flush the buffered\n"
                     "  records, which is todos.txt item 15\n",
                     recovered);
        fail = 1;
    }
    if (rows<NodeIdentity>(dir2) != 1) {
        std::fprintf(stderr, "crash_identity_smoke: SIGSEGV arm lost node_identity\n");
        fail = 1;
    }
    // The dictionary that gives every Handle meaning. It is the risky half of
    // the handler and runs only after the raw records are already durable, so
    // its absence is a softer failure than theirs — but on a clean SIGSEGV with
    // no lock held it must be there.
    if (!fs::exists(dir2 / "intern-blob.bin")) {
        std::fprintf(stderr,
                     "crash_identity_smoke: no intern-blob.bin after SIGSEGV — recovered records\n"
                     "  carrying handles cannot be read\n");
        fail = 1;
    }

    fs::remove_all(base);
    if (fail == 0)
        std::printf("crash_identity_smoke: ok — SIGKILL keeps identity only, SIGSEGV recovers all "
                    "8 records and still dies by SIGSEGV\n");
    return fail;
}
