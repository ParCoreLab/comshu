// FileSink under a filesystem that says no.
//
// WHAT THIS GUARDS. Until 2026-09-03 the sink ignored the return value of every
// fwrite, including the one that writes the StreamHeader. On MN5 with the home
// quota exhausted that produced stream files holding payload with no header in
// front of them, others at zero length, and a process that exited 0. Three suite
// cases went red pointing at InfiniBand and UCX behaviour while the actual fault
// was `gpfs_home` at 83.99 GB against an 80 GB quota — an hour of diagnosis
// spent on the wrong layer because the layer at fault said nothing.
// See plans/PLAN-20260903-0100.
//
// The interesting cases are all failure cases, and a unit test cannot fill a
// filesystem. What it CAN do is take the two write paths away:
//
//   * an unwritable directory, so fopen fails. This also covers a latent crash
//     the rewrite removed — the old code stored the null FILE* in its map and
//     the destructor called fclose(nullptr), which is undefined behaviour on a
//     path that only runs once something has already gone wrong.
//   * a directory that cannot be created at all.
//
// The happy path is checked too, and not as a formality: it is what proves the
// loss counters stay silent when nothing is wrong. A sink that reported losses
// on a healthy run would be turned off within a week.
//
// Runs as root would defeat the permission cases — root writes anywhere — so
// they self-skip rather than pass vacuously.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "comshu/rec/sink.hpp"

using comshu::core::RecordKind;
using comshu::rec::BufferView;
using comshu::rec::FileSink;

namespace fs = std::filesystem;

namespace {

int g_fail = 0;

void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok " : "XX ", what);
    if (!ok)
        g_fail = 1;
}

// 24 bytes of payload as two 12-byte "records". The exact shape does not
// matter; the stride does, because the header records it.
struct Payload {
    std::byte bytes[24]{};
};

BufferView view_of(const Payload& p, std::uint32_t type_id) {
    BufferView v;
    v.type_id = type_id;
    v.kind = RecordKind::Event;
    v.data = p.bytes;
    v.bytes = sizeof p.bytes;
    v.count = 2;
    return v;
}

fs::path temp_dir(const char* tag) {
    auto base = fs::temp_directory_path() /
                ("comshu-sink-" + std::string(tag) + "-" + std::to_string(::getpid()));
    fs::remove_all(base);
    return base;
}

}  // namespace

int main() {
    std::printf("file sink: what happens when the filesystem says no\n");
    const Payload p{};

    // --- the happy path stays quiet -----------------------------------------
    {
        const auto dir = temp_dir("ok");
        {
            FileSink sink(dir);
            sink.submit(view_of(p, 700));
            sink.flush();
            expect(!sink.directory_failed(), "a writable directory is not reported as failed");
            expect(sink.losses().empty(), "a healthy stream reports no loss");
        }
        // header (32) + payload (24). If this is 24 the header write was
        // skipped, which is the exact shape the MN5 captures had.
        std::error_code ec;
        const auto      size = fs::file_size(dir / "700.pcs", ec);
        expect(!ec && size == 56, "the file is header + payload, not payload alone");
        fs::remove_all(dir);
    }

    // --- an unwritable directory --------------------------------------------
    if (::geteuid() == 0) {
        std::printf("  [--- ] permission cases skipped: running as root writes anywhere\n");
    } else {
        const auto dir = temp_dir("nowrite");
        fs::create_directories(dir);
        fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace);

        std::vector<comshu::rec::StreamLoss> losses;
        {
            FileSink sink(dir);
            sink.submit(view_of(p, 701));
            // A SECOND submit of the same type: the first failed to open, and
            // the old code cached a null FILE* and silently dropped everything
            // after it. Both must be counted.
            sink.submit(view_of(p, 701));
            sink.flush();
            losses = sink.losses();
            // Destruction here is the fclose(nullptr) path. Reaching the next
            // line at all is part of what this case checks.
        }
        expect(losses.size() == 1, "one failing stream is reported");
        if (losses.size() == 1) {
            expect(losses[0].type_id == 701, "the loss names the stream that failed");
            expect(losses[0].never_opened, "an fopen failure is reported as never_opened");
            expect(losses[0].lost_records == 4, "both submits are counted, not just the first");
            expect(losses[0].lost_bytes == 48, "and their bytes");
        }
        std::printf("  [ok ] destructor survived a stream that never opened\n");

        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
        fs::remove_all(dir);
    }

    // --- a directory that cannot be created ---------------------------------
    if (::geteuid() != 0) {
        const auto parent = temp_dir("noparent");
        fs::create_directories(parent);
        fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace);
        {
            FileSink sink(parent / "child");
            expect(sink.directory_failed(), "an uncreatable output directory is reported");
        }
        fs::permissions(parent, fs::perms::owner_all, fs::perm_options::replace);
        fs::remove_all(parent);
    }

    // --- an empty view still creates nothing --------------------------------
    //
    // Not a failure case, but adjacent to one: a zero-count view has no stride,
    // so a header written from it would frame the stream as stride 0 and no
    // reader could use it. The sink must decline rather than create the file.
    {
        const auto dir = temp_dir("empty");
        {
            FileSink   sink(dir);
            BufferView v;
            v.type_id = 702;
            v.kind = RecordKind::Event;
            v.data = p.bytes;
            v.bytes = 0;
            v.count = 0;
            sink.submit(v);
            expect(sink.losses().empty(), "an empty view is not a loss");
        }
        expect(!fs::exists(dir / "702.pcs"), "an empty view creates no file to misframe");
        fs::remove_all(dir);
    }

    std::printf("%s\n", g_fail ? "FAIL" : "PASS — the sink reports what it could not write");
    return g_fail;
}
