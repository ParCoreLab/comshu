// pcomm-read — post-processor: raw per-type streams + intern sidecars ->
// TimeAligner -> columnar Tables -> exporters (Parquet canonical, Perfetto
// derived; OTF2/CTF optional). Heavy deps (Arrow) are allowed here — this is
// off the hot path. See ../../ARCHITECTURE.md §9.
//
// Skeleton: stream inventory only, so capture output can be inspected today.

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: pcomm-read <trace-dir> [-o out.parquet] [--perfetto out.pftrace]\n",
                   stderr);
        return 1;
    }

    namespace fs = std::filesystem;
    fs::path dir = argv[1];
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm-read: not a directory: %s\n", dir.c_str());
        return 1;
    }

    std::printf("streams in %s:\n", dir.c_str());
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".pcs") {
            std::printf("  %-40s %10ju bytes\n",
                        fs::relative(entry.path(), dir).c_str(),
                        static_cast<std::uintmax_t>(entry.file_size()));
        }
    }
    // TODO(phase 1): resolve interning, decode per-type streams via
    // RecordTrait, TimeAligner (offset+skew fit from ClockAnchor artifacts),
    // Parquet + Perfetto exporters.
    return 0;
}
